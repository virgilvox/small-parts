/* world.h: the lab. HeatSync Labs at 108 West Main, built from the HSL-108
 * model at toy scale (gen/lab_areas.h: 1 m of lab is LAB_SCALE units, the
 * hero is 1.8 units, 9 cm, tall), collision against every triangle of it,
 * and the missions' places in it.
 *
 * Axes: x runs from the street (x = 0, the front windows) to the back of
 * the building (x = 808, the kitchen and the metal fab bay); z runs across
 * the room, 0 at the west wall (printers, sewing, the lasers) to 188 at the
 * east wall (coworking, electronics, the woodshop); y is up, the floor is 0,
 * the ceiling about 60. */
#ifndef SP_WORLD_H
#define SP_WORLD_H
#include "ds.h"
#include "three.h"
#include "gen/lab_areas.h"

void world_init(t3_scene *scene);   /* once: load the lab and build its collision */

/* Move a standing cylinder (feet at p->y, radius r, height h) by vel*dt
 * against the lab. Slides along walls and furniture, lands on floors and
 * tops (sets *grounded and zeroes vel->y), bumps heads, steps up ledges
 * lower than WORLD_STEP. */
#define WORLD_STEP 0.55f
void world_move(v3 *p, v3 *vel, float r, float h, float dt, bool *grounded);
/* first hit of a ray against the lab; *t is the distance, *normal faces the ray */
bool world_raycast(v3 origin, v3 dir, float max_dist, float *t, v3 *normal);
/* true when nothing in the lab is between a and b */
bool world_visible(v3 a, v3 b);
/* height of the first surface at or below p (searching 200 units down), or -1e9 */
float world_floor_y(v3 p);
/* a sphere against the lab: pushes *c out of everything it overlaps; true if it touched */
bool world_push_sphere(v3 *c, float r, v3 *normal);

/* the model's area tags (gen/lab_areas.h AREA_*) */
v3 world_area_center(int area);            /* on the floor at the box's centre */
int world_area_at(v3 p);                   /* the smallest area box holding p, -1 if none */
const char *world_area_name(int area);

/* the lab's bounds, for the kill plane and the map */
#define WORLD_MIN_X 0.f
#define WORLD_MAX_X 808.f
#define WORLD_MIN_Z 0.f
#define WORLD_MAX_Z 188.f
#define WORLD_CEIL 60.f

void world_update(float dt, v3 player);
void world_debug_counts(int *tris, int *cells);

#endif
