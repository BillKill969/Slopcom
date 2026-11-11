#include "damage.h"
#include <stddef.h>

void damage_list_clear(DamageList *dl) {
    if (dl) dl->count = 0;
}

void damage_list_add(DamageList *dl, int x, int y, int w, int h) {
    if (!dl || w <= 0 || h <= 0) return;

    /* Check if inside existing rect */
    for (int i = 0; i < dl->count; ++i) {
        DamageRect *r = &dl->rects[i];
        if (x >= r->x && y >= r->y &&
            (x + w) <= (r->x + r->width) &&
            (y + h) <= (r->y + r->height)) {
            return;
        }
    }

    /* Check if superset of existing rect */
    for (int i = 0; i < dl->count; ++i) {
        DamageRect *r = &dl->rects[i];
        if (r->x >= x && r->y >= y &&
            (r->x + r->width) <= (x + w) &&
            (r->y + r->height) <= (y + h)) {
            r->x = x; r->y = y; r->width = w; r->height = h;
            return;
        }
    }

    if (dl->count < MAX_DAMAGE_RECTS) {
        dl->rects[dl->count++] = (DamageRect){ x, y, w, h };
        return;
    }

    /* If capacity reached, merge with closest */
    int min_area_grow = -1;
    int best_idx = 0;
    for (int i = 0; i < dl->count; ++i) {
        DamageRect *r = &dl->rects[i];
        int nx1 = (r->x < x) ? r->x : x;
        int ny1 = (r->y < y) ? r->y : y;
        int nx2 = ((r->x + r->width) > (x + w)) ? (r->x + r->width) : (x + w);
        int ny2 = ((r->y + r->height) > (y + h)) ? (r->y + r->height) : (y + h);
        int area_grow = (nx2 - nx1) * (ny2 - ny1) - (r->width * r->height);
        if (min_area_grow < 0 || area_grow < min_area_grow) {
            min_area_grow = area_grow;
            best_idx = i;
        }
    }
    DamageRect *r = &dl->rects[best_idx];
    int nx1 = (r->x < x) ? r->x : x;
    int ny1 = (r->y < y) ? r->y : y;
    int nx2 = ((r->x + r->width) > (x + w)) ? (r->x + r->width) : (x + w);
    int ny2 = ((r->y + r->height) > (y + h)) ? (r->y + r->height) : (y + h);
    r->x = nx1; r->y = ny1; r->width = nx2 - nx1; r->height = ny2 - ny1;
}

void damage_list_union(DamageList *dst, const DamageList *src) {
    if (!dst || !src) return;
    for (int i = 0; i < src->count; ++i) {
        damage_list_add(dst, src->rects[i].x, src->rects[i].y, src->rects[i].width, src->rects[i].height);
    }
}

bool damage_rect_intersect(const DamageRect *a, const DamageRect *b,
                           DamageRect *out) {
    if (!a || !b || a->width <= 0 || a->height <= 0 ||
        b->width <= 0 || b->height <= 0) {
        return false;
    }
    int x1 = (a->x > b->x) ? a->x : b->x;
    int y1 = (a->y > b->y) ? a->y : b->y;
    int ax2 = a->x + a->width;
    int ay2 = a->y + a->height;
    int bx2 = b->x + b->width;
    int by2 = b->y + b->height;
    int x2 = (ax2 < bx2) ? ax2 : bx2;
    int y2 = (ay2 < by2) ? ay2 : by2;

    if (x2 <= x1 || y2 <= y1) {
        return false;
    }
    if (out) {
        out->x = x1;
        out->y = y1;
        out->width = x2 - x1;
        out->height = y2 - y1;
    }
    return true;
}
