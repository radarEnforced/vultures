/* Copyright (c) Daniel Thaler, 2006, 2008                        */
/* NetHack may be freely redistributed.  See license for details. */

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "hack.h"

#if !defined WIN32
    #include <sys/mman.h>
#endif

#include "vultures_types.h"
#include "vultures_tile.h"
#include "vultures_gra.h"
#include "vultures_gfl.h"
#include "vultures_map.h"
#include "vultures_gen.h"
#include "vultures_opt.h"
#include "vultures_tileconfig.h"


#define TILEARRAYLEN (GAMETILECOUNT*3)

#define TILECACHE_MAXAGE 4

typedef struct {
    vultures_tile *tile;
    int age;
} vultures_tilecache_entry;


/* main tile arrays */
static vultures_tilecache_entry *vultures_tilecache;

/* semi-transparent black areas used to shade floortiles */
static SDL_Surface * vultures_ftshade1;
static SDL_Surface * vultures_ftshade2;


static vultures_tile *vultures_make_alpha_player_tile(int monnum, double op_scale);
static inline vultures_tile * vultures_shade_tile(vultures_tile *orig, int shadelevel);
static inline void vultures_set_tile_alpha(vultures_tile *tile, double opacity);



inline static void vultures_free_tile(vultures_tile *tile)
{
    SDL_FreeSurface(tile->graphic);
    free(tile);
}


/* flip the tile arrays and unload all tiles that were not used for 2 turns */
void vultures_tilecache_discard(void)
{
    int i;
    for (i = 0; i < TILEARRAYLEN; i++)
    {
        if (vultures_tilecache[i].tile)
            vultures_free_tile(vultures_tilecache[i].tile);
        vultures_tilecache[i].tile = NULL;
        vultures_tilecache[i].age = 0;
    }
}

void vultures_tilecache_age(void)
{
    int i;
    for (i = 0; i < TILEARRAYLEN; i++)
    {
        vultures_tilecache[i].age++;
        if (vultures_tilecache[i].tile && vultures_tilecache[i].age > TILECACHE_MAXAGE)
        {
            vultures_free_tile(vultures_tilecache[i].tile);
            vultures_tilecache[i].tile = NULL;
        }
    }
}

void vultures_tilecache_add(vultures_tile *tile, int tile_id)
{
    if (vultures_tilecache[tile_id].tile && vultures_tilecache[tile_id].tile != tile)
        vultures_free_tile(vultures_tilecache[tile_id].tile);

    vultures_tilecache[tile_id].tile = tile;
    vultures_tilecache[tile_id].age = 0;
}


vultures_tile *vultures_tilecache_get(int tile_id)
{
    vultures_tilecache[tile_id].age = 0;
    if (vultures_tilecache[tile_id].tile)
        return vultures_tilecache[tile_id].tile;
    return NULL;
}


void vultures_put_tile_shaded(int x, int y, int tile_id, int shadelevel)
{
    vultures_tile * tile = NULL;

    if (tile_id < 0)
        return;

    tile = vultures_get_tile_shaded(tile_id, shadelevel);

    if (tile != NULL)
        vultures_put_img(x + tile->xmod, y + tile->ymod, tile->graphic);
}


/* vultures_get_tile is responsible for tile "administration"
 * if the tile is already loaded it will return a pointer to it
 * otherwise it loads the tile, stores the pointer and returns it */
vultures_tile * vultures_get_tile_shaded(int tile_id, int shadelevel)
{
    vultures_tile *tile;
    int shaded_id = tile_id + shadelevel * GAMETILECOUNT;

    if (tile_id < 0)
        return NULL;

    /* modifiying the tile_id: must come first */
    /* if we have an object, we manipulate the tile id to give shuffled objects */
    if (TILE_IS_OBJECT(tile_id))
        tile_id = objects[tile_id].oc_descr_idx;
    else if (TILE_IS_OBJICON(tile_id))
        tile_id = objects[tile_id - ICOTILEOFFSET].oc_descr_idx + ICOTILEOFFSET;

    /* if the tile is merely a pointer to another tile we modify the tile_id */
    if (vultures_gametiles[tile_id].ptr != -1)
        tile_id = vultures_gametiles[tile_id].ptr;


    /* specialized load functions: second */
    /* if you are invisible you have the V_TILE_PLAYER_INVIS. here we give that tile a meaning */
    if (tile_id == V_MISC_PLAYER_INVIS)
        tile = vultures_make_alpha_player_tile(u.umonnum, canseeself() ? 0.6 : 0.35);

    else
        /* if shadelevel == 0 then shaded_id == tile_id */
        tile = vultures_tilecache_get(shaded_id);

    if (!tile) /* never true if tile_id == V_MISC_PLAYER_INVIS */
    {
        if (shadelevel > 0)
        {
            tile = vultures_get_tile_shaded(tile_id, 0);
            tile = vultures_shade_tile(tile, shadelevel);
        }
        else
            tile = vultures_load_tile(tile_id);
    }

    vultures_tilecache_add(tile, shaded_id);

    return tile;
}


/* vultures_load_tile is the actual tile loader
 * it returns a pointer to the tile; the caller is expected to free it */
vultures_tile * vultures_load_tile(int tile_id)
{
    vultures_tile * newtile;
    char * data;
    FILE *fp;
    int fsize;

    /* if data_len is 0 the tile doesn't have a graphic */
    if (!vultures_gametiles[tile_id].filename)
        return NULL;

    fp = fopen(vultures_gametiles[tile_id].filename, "rb");
    if (!fp)
        return NULL;

    /* obtain file size. */
    fseek(fp, 0, SEEK_END);
    fsize = ftell(fp);
    rewind(fp);

    /* load the tile */
    data = malloc(fsize);
    fread(data, fsize, 1, fp);

    newtile = malloc(sizeof(vultures_tile));
    if (!newtile)
    {
        free(data);
        return NULL;
    }

    newtile->graphic = vultures_load_surface(data, fsize);
    newtile->xmod = vultures_gametiles[tile_id].hs_x;
    newtile->ymod = vultures_gametiles[tile_id].hs_y;

    free(data);
    fclose(fp);

    if (TILE_IS_WALL(tile_id))
        vultures_set_tile_alpha(newtile, vultures_opts.wall_opacity);

    return newtile;
}


/* darken a tile; the amount of darkening is determined by the tile_id */
static inline vultures_tile * vultures_shade_tile(vultures_tile *orig, int shadelevel)
{
    SDL_Surface * blend = (shadelevel == 1) ? vultures_ftshade1 : vultures_ftshade2;
    vultures_tile *tile = malloc(sizeof(vultures_tile));

    tile->xmod = orig->xmod;
    tile->ymod = orig->ymod;
    tile->graphic = vultures_get_img_src(0,0, orig->graphic->w-1, orig->graphic->h-1, orig->graphic);
    SDL_BlitSurface(blend, NULL, tile->graphic, NULL);

    return tile;
}



void vultures_put_tilehighlight(int x, int y, int tile_id)
{
    vultures_tile *tile;
    unsigned int *srcdata, *destdata, alpha;
    SDL_Surface *highlight;
    SDL_PixelFormat *pxf;
    int i, j;

    if (tile_id < 0)
        return;

    /* get the base tile */
    tile = vultures_get_tile_shaded(tile_id, 0);

    pxf = tile->graphic->format;

    highlight = SDL_CreateRGBSurface(SDL_SWSURFACE | SDL_SRCALPHA, tile->graphic->w, tile->graphic->h,
                                     pxf->BitsPerPixel, pxf->Rmask, pxf->Gmask, pxf->Bmask, pxf->Amask);

    srcdata = tile->graphic->pixels;
    destdata = highlight->pixels;

    for (i = 0; i < highlight->h; i++)
    {
        for (j = 0; j < highlight->w; j++)
        {
            alpha = (srcdata[i * highlight->w + j] & pxf->Amask) >> pxf->Ashift;
            /*respect src transparency, but cap it at 50% */
            alpha = (alpha < 0x80) ? alpha : 0x80;

            if (srcdata[i * highlight->w + j] & ~(pxf->Amask))
            {
                destdata[i * highlight->w + j] |= alpha << pxf->Ashift;
                destdata[i * highlight->w + j] |= 0x20 << pxf->Rshift;
                destdata[i * highlight->w + j] |= 0x80 << pxf->Gshift;
                destdata[i * highlight->w + j] |= 0xff << pxf->Bshift;
            }
        }
    }

    /* draw the highlight */
    vultures_put_img(x + tile->xmod, y + tile->ymod, highlight);

    /* free the highlight. no caching, as this shouldn't be a hot path */
    SDL_FreeSurface(highlight);
}




int vultures_load_gametiles(void)
{
    char * filename;
    FILE * fp;

    /* load gametiles.bin */
    filename = vultures_make_filename(V_CONFIG_DIRECTORY, NULL, "vultures_tiles.conf");
    fp = fopen(filename, "rb");
    free(filename);
    if (!fp)
    {
        printf("FATAL: Could not read tile configuration (vultures_tiles.conf) file: %s", strerror(errno));
        exit(1);
    }

    vultures_parse_tileconf(fp);

    fclose(fp);

    /* initialize the two tile arrays. must happen after reading the config file,
     * as GAMETILECOUNT and TILEARRAYLEN are not know before */
    vultures_tilecache = malloc(TILEARRAYLEN * sizeof(vultures_tilecache_entry));
    memset(vultures_tilecache, 0, TILEARRAYLEN * sizeof(vultures_tilecache_entry));


    /* create the surfaces used to shade floor tiles */
    vultures_ftshade1 = SDL_CreateRGBSurface(SDL_SWSURFACE | SDL_SRCALPHA, 112, 100, 32,
                                  vultures_px_format->Rmask, vultures_px_format->Gmask,
                                  vultures_px_format->Bmask, DEF_AMASK);
    vultures_ftshade2 = SDL_CreateRGBSurface(SDL_SWSURFACE | SDL_SRCALPHA, 112, 100, 32,
                                  vultures_px_format->Rmask, vultures_px_format->Gmask,
                                  vultures_px_format->Bmask, DEF_AMASK);
    SDL_FillRect(vultures_ftshade1, NULL, CLR32_BLACK_A30);
    SDL_FillRect(vultures_ftshade2, NULL, CLR32_BLACK_A70);

    return 1;
}


void vultures_unload_gametiles(void)
{
    int i;

    vultures_tilecache_discard();
    free(vultures_tilecache);

    SDL_FreeSurface(vultures_ftshade1);
    SDL_FreeSurface(vultures_ftshade2);

    for (i = 0; i < GAMETILECOUNT; i++)
    {
        if (vultures_gametiles[i].filename)
            free(vultures_gametiles[i].filename);
    }
    free(vultures_gametiles);
}



static vultures_tile *vultures_make_alpha_player_tile(int monnum, double op_scale)
{
    static int tilenum = -1;
    static double lastscale = 0;
    vultures_tile *montile;
    vultures_tile *tile = NULL;

    tile = vultures_tilecache_get(V_MISC_PLAYER_INVIS);

    /* monnum may change if player polymorphs... */
    if (monnum != tilenum || lastscale != op_scale || !tile)
    {
        lastscale = op_scale;

        montile = vultures_get_tile(MONSTER_TO_VTILE(monnum));
        tile = malloc(sizeof(*tile));
        if (tile == NULL)
            return montile;

        /* set tile->graphic to be an exact copy of montile->graphic */
        tile->graphic = vultures_get_img_src(0, 0, montile->graphic->w-1,
                                 montile->graphic->h-1, montile->graphic);
        tile->xmod = montile->xmod;
        tile->ymod = montile->ymod;

        vultures_set_tile_alpha(tile, op_scale);

        tilenum = monnum;
    }

    return tile;
}

/* makes a tile (partially) transparent */
static inline void vultures_set_tile_alpha(vultures_tile *tile, double opacity)
{
    unsigned char * rawdata;
    int x, y;

    /* scale opacity of every pixel. This works nicely, because
     * complete transparency has a numeric value of 0, so it will remain unchanged,
     * while all other pixels transparency will depend on their current transparency */
    rawdata = tile->graphic->pixels;
    for (y = 0; y < tile->graphic->h; y++)
        for (x = 0; x < tile->graphic->pitch; x += 4)
            /* multiply the alpha component by the opacity */
            rawdata[y*tile->graphic->pitch+x+3] *= opacity;
}




