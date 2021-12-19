void sphere_bounds_func(const struct RTCBoundsFunctionArguments* args) {
    const Sphere *sphere = (const Sphere*) args->geometryUserPtr;
    RTCBounds* bounds_o = args->bounds_o;
    bounds_o->lower_x = sphere->position.x - sphere->radius;
    bounds_o->lower_y = sphere->position.y - sphere->radius;
    bounds_o->lower_z = sphere->position.z - sphere->radius;
    bounds_o->upper_x = sphere->position.x + sphere->radius;
    bounds_o->upper_y = sphere->position.y + sphere->radius;
    bounds_o->upper_z = sphere->position.z + sphere->radius;
}

/// Numerically stable quadratic equation solver at^2 + bt + c = 0
/// See https://people.csail.mit.edu/bkph/articles/Quadratics.pdf
/// returns false when it can't find solutions.
bool solve_quadratic(Real a, Real b, Real c, Real *t0, Real *t1) {
    // Degenerated case
    if (a == 0) {
        if (b == 0) {
            return false;
        }
        *t0 = *t1 = -c / b;
        return true;
    }

    Real discriminant = b * b - 4 * a * c;
    if (discriminant < 0) {
        return false;
    }
    Real root_discriminant = sqrt(discriminant);
    if (b >= 0) {
        *t0 = (- b - root_discriminant) / (2 * a);
        *t1 = 2 * c / (- b - root_discriminant);
    } else {
        *t0 = 2 * c / (- b + root_discriminant);
        *t1 = (- b + root_discriminant) / (2 * a);
    }
    return true;
}

void sphere_intersect_func(const RTCIntersectFunctionNArguments* args) {
    assert(args->N == 1);
    int *valid = args->valid;
    if (!valid[0]) {
        return;
    }
    void *ptr = args->geometryUserPtr;
    const Sphere *sphere = (const Sphere*)ptr;
    RTCRayHitN *rayhit = args->rayhit;
    RTCRay *rtc_ray = (RTCRay*)RTCRayHitN_RayN(rayhit, 1);
    RTCHit *rtc_hit = (RTCHit*)RTCRayHitN_HitN(rayhit, 1);

    Ray ray{Vector3{rtc_ray->org_x, rtc_ray->org_y, rtc_ray->org_z},
            Vector3{rtc_ray->dir_x, rtc_ray->dir_y, rtc_ray->dir_z},
            rtc_ray->tnear, rtc_ray->tfar};
    // Our sphere is ||p - x||^2 = r^2
    // substitute x = o + d * t, we want to solve for t
    // ||p - (o + d * t)||^2 = r^2
    // (p.x - (o.x + d.x * t))^2 + (p.y - (o.y + d.y * t))^2 + (p.z - (o.z + d.z * t))^2 - r^2 = 0
    // (d.x^2 + d.y^2 + d.z^2) t^2 + 2 * (d.x * (o.x - p.x) + d.y * (o.y - p.y) + d.z * (o.z - p.z)) t + 
    // ((p.x-o.x)^2 + (p.y-o.y)^2 + (p.z-o.z)^2  - r^2) = 0
    // A t^2 + B t + C
    Vector3 v = ray.org - sphere->position;
    Real A = dot(ray.dir, ray.dir);
    Real B = 2 * dot(ray.dir, v);
    Real C = dot(v, v) - sphere->radius * sphere->radius;
    Real t0, t1;
    if (!solve_quadratic(A, B, C, &t0, &t1)) {
        // No intersection
        return;
    }
    assert(t0 <= t1);
    Real t = -1;
    if (t0 >= ray.tnear && t0 < ray.tfar) {
        t = t0;
    }
    if (t1 >= ray.tnear && t1 < ray.tfar && t < 0) {
        t = t1;
    }

    if (t >= ray.tnear && t < ray.tfar) {
        // Record the intersection
        Vector3 p = ray.org + t0 * ray.dir;
        Vector3 geometry_normal = p - sphere->position;
        rtc_hit->Ng_x = geometry_normal.x;
        rtc_hit->Ng_y = geometry_normal.y;
        rtc_hit->Ng_z = geometry_normal.z;
        // We use the spherical coordinates as uv
        Vector3 cartesian = geometry_normal / sphere->radius;
        // https://en.wikipedia.org/wiki/Spherical_coordinate_system#Cartesian_coordinates
        // We use the convention that y is up axis.
        Real elevation = acos(cartesian.y);
        Real azimuth = atan2(cartesian.z, cartesian.x);
        rtc_hit->u = azimuth / c_TWOPI;
        rtc_hit->v = elevation / c_PI;
        rtc_hit->primID = args->primID;
        rtc_hit->geomID = args->geomID;
        rtc_hit->instID[0] = args->context->instID[0];
        rtc_ray->tfar = t0;
    }
}

void sphere_occluded_func(const RTCOccludedFunctionNArguments* args) {
    assert(args->N == 1);
    int *valid = args->valid;
    if (!valid[0]) {
        return;
    }

    void *ptr = args->geometryUserPtr;
    const Sphere *sphere = (const Sphere*)ptr;
    RTCRay *rtc_ray = (RTCRay *)args->ray;
    
    Ray ray{Vector3{rtc_ray->org_x, rtc_ray->org_y, rtc_ray->org_z},
            Vector3{rtc_ray->dir_x, rtc_ray->dir_y, rtc_ray->dir_z},
            rtc_ray->tnear, rtc_ray->tfar};

    // See sphere_intersect_func for explanation.
    Vector3 v = ray.org - sphere->position;
    Real A = dot(ray.dir, ray.dir);
    Real B = 2 * dot(ray.dir, v);
    Real C = dot(v, v) - sphere->radius * sphere->radius;
    Real t0, t1;
    if (!solve_quadratic(A, B, C, &t0, &t1)) {
        // No intersection
        return;
    }

    assert(t0 <= t1);
    Real t = -1;
    if (t0 >= ray.tnear && t0 < ray.tfar) {
        t = t0;
    }
    if (t1 >= ray.tnear && t1 < ray.tfar && t < 0) {
        t = t1;
    }

    if (t >= ray.tnear && t < ray.tfar) {
        rtc_ray->tfar = -infinity<float>();
    }
}

uint32_t register_embree_op::operator()(const Sphere &sphere) const {
    RTCGeometry rtc_geom = rtcNewGeometry(device, RTC_GEOMETRY_TYPE_USER);
    uint32_t geomID = rtcAttachGeometry(scene, rtc_geom);
    rtcSetGeometryUserPrimitiveCount(rtc_geom, 1);
    rtcSetGeometryUserData(rtc_geom, (void *)&sphere);
    rtcSetGeometryBoundsFunction(rtc_geom, sphere_bounds_func, nullptr);
    rtcSetGeometryIntersectFunction(rtc_geom, sphere_intersect_func);
    rtcSetGeometryOccludedFunction(rtc_geom, sphere_occluded_func);
    rtcCommitGeometry(rtc_geom);
    rtcReleaseGeometry(rtc_geom);
    return geomID;
}

PointAndNormal sample_point_on_shape_op::operator()(const Sphere &sphere) const {
    // https://www.pbr-book.org/3ed-2018/Monte_Carlo_Integration/2D_Sampling_with_Multidimensional_Transformations#UniformSampleSphere
    Real z = 1 - 2 * uv.x;
    Real r = sqrt(fmax(Real(0), 1 - z * z));
    Real phi = 2 * c_PI * uv.y;
    Vector3 offset(r * cos(phi), r * sin(phi), z);
    Vector3 position = sphere.position + sphere.radius * offset;
    Vector3 normal = offset;
    return PointAndNormal{position, normal};
}

Real surface_area_op::operator()(const Sphere &sphere) const {
    return 4 * c_PI * sphere.radius * sphere.radius;
}

Real pdf_point_on_shape_op::operator()(const Sphere &sphere) const {
    return 1 / surface_area_op{}(sphere);
}

void init_sampling_dist_op::operator()(Sphere &sphere) const {
}

ShadingInfo compute_shading_info_op::operator()(const Sphere &sphere) const {
    // To compute the shading frame, we use the geometry normal as normal,
    // and dpdu as one of the tangent vector. 
    // We use the azimuthal angle as u, and the elevation as v, 
    // thus the point p on sphere and u, v has the following relationship:
    // p = center + {r * cos(u) * sin(v), r * sin(u) * sin(v), r * cos(v)}
    // thus dpdu = {-r * sin(u) * sin(v), r * cos(u) * sin(v), 0}
    //      dpdv = { r * cos(u) * cos(v), r * sin(u) * cos(v), - r * sin(v)}
    Vector3 dpdu{-sphere.radius * sin(vertex.st[0]) * sin(vertex.st[1]),
                  sphere.radius * cos(vertex.st[0]) * sin(vertex.st[1]),
                 Real(0)};
    Vector3 dpdv{ sphere.radius * cos(vertex.st[0]) * cos(vertex.st[1]),
                  sphere.radius * sin(vertex.st[0]) * cos(vertex.st[1]),
                 -sphere.radius * sin(vertex.st[1])};
    // normalize for shading frame calculation
    Vector3 tangent = normalize(dpdu);
    Frame shading_frame(tangent,
                        normalize(cross(vertex.geometry_normal, tangent)),
                        vertex.geometry_normal);
    return ShadingInfo{vertex.st,
                       shading_frame,
                       1 / sphere.radius, /* mean curvature */
                       (length(dpdu) + length(dpdv)) / 2};
}