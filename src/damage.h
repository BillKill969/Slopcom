#ifndef SLOPCOM_DAMAGE_H
#define SLOPCOM_DAMAGE_H

#include <stdbool.h>

#define MAX_DAMAGE_RECTS 32
#define DAMAGE_HISTORY_CAP 4

typedef struct {
    int x, y, width, height;
} DamageRect;

typedef struct {
    DamageRect rects[MAX_DAMAGE_RECTS];
    int count;
} DamageList;

void damage_list_clear(DamageList *list);
void damage_list_add(DamageList *list, int x, int y, int width, int height);
void damage_list_union(DamageList *dst, const DamageList *src);
bool damage_rect_intersect(const DamageRect *a, const DamageRect *b,
                           DamageRect *out);

#endif /* SLOPCOM_DAMAGE_H */
