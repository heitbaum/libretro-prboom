/* Emacs style mode select   -*- C++ -*-
 *-----------------------------------------------------------------------------
 *
 *
 *  PrBoom: a Doom port merged with LxDoom and LSDLDoom
 *  based on BOOM, a modified and improved DOOM engine
 *  Copyright (C) 1999 by
 *  id Software, Chi Hoang, Lee Killough, Jim Flynn, Rand Phares, Ty Halderman
 *  Copyright (C) 1999-2004 by
 *  Jess Haas, Nicolas Kalkhof, Colin Phipps, Florian Schulze
 *  Copyright 2005, 2006 by
 *  Florian Schulze, Colin Phipps, Neil Stevens, Andrey Budko
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 *  02111-1307, USA.
 *
 * DESCRIPTION:
 *      All the clipping: columns, horizontal spans, sky columns.
 *
 *-----------------------------------------------------------------------------*/
//
// 4/25/98, 5/2/98 killough: reformatted, beautified

#include "doomstat.h"
#include "r_main.h"
#include "r_bsp.h"
#include "r_segs.h"
#include "r_plane.h"
#include "r_things.h"
#include "r_draw.h"
#include "w_wad.h"
#include "v_video.h"
#include "lprintf.h"

// OPTIMIZE: closed two sided lines as single sided

// killough 1/6/98: replaced globals with statics where appropriate

// True if any of the segs textures might be visible.
static dbool    segtextured;
static dbool    markfloor;      // False if the back side is the same plane.
static dbool    markceiling;
static dbool    maskedtexture;
static int      toptexture;
static int      bottomtexture;
static int      midtexture;

dbool           r_wiggle_fix = 0;

static fixed_t  toptexheight, midtexheight, bottomtexheight; // cph

angle_t         rw_normalangle; // angle to line origin
int             rw_angle1;
fixed_t         rw_distance;

//
// regular wall
//
static int      rw_x;
static int      rw_stopx;
static angle_t  rw_centerangle;
static fixed_t  rw_offset;
static fixed_t  rw_scale;
static fixed_t  rw_scalestep;
static fixed_t  rw_midtexturemid;
static fixed_t  rw_toptexturemid;
static fixed_t  rw_bottomtexturemid;
static int      rw_lightlevel;
static int      worldtop;
static int      worldbottom;
static int      worldhigh;
static int      worldlow;
static fixed_t  pixhigh;
static fixed_t  pixlow;
static fixed_t  pixhighstep;
static fixed_t  pixlowstep;
static fixed_t  topfrac;
static fixed_t  topstep;
static fixed_t  bottomfrac;
static fixed_t  bottomstep;
static int      *maskedtexturecol; // dropoff overflow

//
// R_FixWiggle()
// Dynamic wall/texture rescaler, AKA "WiggleHack II"
//  by Kurt "kb1" Baumgardner ("kb") and Andrey "Entryway" Budko ("e6y")
//
//  [kb] When the rendered view is positioned, such that the viewer is
//   looking almost parallel down a wall, the result of the scale
//   calculation in R_ScaleFromGlobalAngle becomes very large. And, the
//   taller the wall, the larger that value becomes. If these large
//   values were used as-is, subsequent calculations would overflow,
//   causing full-screen HOM, and possible program crashes.
//
//  Therefore, vanilla Doom clamps this scale calculation, preventing it
//   from becoming larger than 0x400000 (64*FRACUNIT). This number was
//   chosen carefully, to allow reasonably-tight angles, with reasonably
//   tall sectors to be rendered, within the limits of the fixed-point
//   math system being used. When the scale gets clamped, Doom cannot
//   properly render the wall, causing an undesirable wall-bending
//   effect that I call "floor wiggle". Not a crash, but still ugly.
//
//  Modern source ports offer higher video resolutions, which worsens
//   the issue. And, Doom is simply not adjusted for the taller walls
//   found in many PWADs.
//
//  This code attempts to correct these issues, by dynamically
//   adjusting the fixed-point math, and the maximum scale clamp,
//   on a wall-by-wall basis. This has 2 effects:
//
//  1. Floor wiggle is greatly reduced and/or eliminated.
//  2. Overflow is no longer possible, even in levels with maximum
//     height sectors (65535 is the theoretical height, though Doom
//     cannot handle sectors > 32767 units in height.
//
//  The code is not perfect across all situations. Some floor wiggle can
//   still be seen, and some texture strips may be slightly misaligned in
//   extreme cases. These effects cannot be corrected further, without
//   increasing the precision of various renderer variables, and, 
//   possibly, creating a noticable performance penalty.
//   

static int			max_rwscale = 64 * FRACUNIT;
static int			heightbits = 12;
static int			heightunit = (1 << 12);
static int			invhgtbits = 4;
 
static const struct
{
	int clamp;
	int heightbits;
}	
	scale_values[8] = {
		{2048 * FRACUNIT, 12}, {1024 * FRACUNIT, 12},
		{1024 * FRACUNIT, 11}, { 512 * FRACUNIT, 11},
		{ 512 * FRACUNIT, 10}, { 256 * FRACUNIT, 10},
		{ 256 * FRACUNIT,  9}, { 128 * FRACUNIT,  9}
	
};

void R_FixWiggle (sector_t *sector)
{
	static int	lastheight = 0;
	int		height = (sector->ceilingheight - sector->floorheight) >> FRACBITS;

	// disallow negative heights. using 1 forces cache initialization
	if (height < 1)
		height = 1;

	// early out?
	if (height != lastheight)
	{
		lastheight = height;

		// initialize, or handle moving sector
		if (height != sector->cachedheight)
		{
			sector->cachedheight = height;
			sector->scaleindex = 0;
			height >>= 7;

			// calculate adjustment
			while (height >>= 1)
				sector->scaleindex++;
		}

		// fine-tune renderer for this wall
		max_rwscale = scale_values[sector->scaleindex].clamp;
		heightbits = scale_values[sector->scaleindex].heightbits;
		heightunit = (1 << heightbits);
		invhgtbits = FRACBITS - heightbits;
	}
}

//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale
//  for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
// killough 5/2/98: reformatted, cleaned up
// CPhipps - moved here from r_main.c

static fixed_t R_ScaleFromGlobalAngle(angle_t visangle)
{
   int     anglea = ANG90 + (visangle-viewangle);
   int     angleb = ANG90 + (visangle-rw_normalangle);
   int     den = FixedMul(rw_distance, finesine[anglea>>ANGLETOFINESHIFT]);
   // proff 11/06/98: Changed for high-res
   fixed_t num = FixedMul(projectiony, finesine[angleb>>ANGLETOFINESHIFT]);
   fixed_t scale;

   if (den > (num >> 16))
   {
      scale = FixedDiv(num, den);

      // [kb] When this evaluates True, the scale is clamped,
		//  and there will be some wiggling.
      if (scale > max_rwscale)
         scale = max_rwscale;
      else if (scale <  256)
         scale = 256;
   }
   else
      scale = max_rwscale;

   return scale;
}

//
// R_RenderMaskedSegRange
//

void R_RenderMaskedSegRange(drawseg_t *ds, int x1, int x2)
{
   int      texnum;
   sector_t tempsec;      // killough 4/13/98
   const rpatch_t *patch;
   R_DrawColumn_f colfunc;
   draw_column_vars_t dcvars;
   angle_t angle;

   R_SetDefaultDrawColumnVars(&dcvars);

   /* Calculate light table.
    * Use different light tables
    * for horizontal / vertical / diagonal. Diagonal?
    */
   curline     = ds->curline;  // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
   colfunc     = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD, drawvars.filterwall, drawvars.filterz);
   frontsector = curline->frontsector;
   backsector  = curline->backsector;
   texnum      = curline->sidedef->midtexture;

   if (!comp[comp_maskedanim])
      texnum = texturetranslation[texnum];

   // killough 4/13/98: get correct lightlevel for 2s normal textures
   rw_lightlevel = R_FakeFlat(frontsector, &tempsec, NULL, NULL, FALSE) ->lightlevel;

   maskedtexturecol = ds->maskedtexturecol;

   rw_scalestep = ds->scalestep;
   spryscale    = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
   mfloorclip   = ds->sprbottomclip;
   mceilingclip = ds->sprtopclip;

   /* find positioning */
   if (curline->linedef->flags & ML_DONTPEGBOTTOM)
   {
      dcvars.texturemid = frontsector->floorheight > backsector->floorheight
         ? frontsector->floorheight : backsector->floorheight;
      dcvars.texturemid = dcvars.texturemid + textureheight[texnum] - viewz;
   }
   else
   {
      dcvars.texturemid =frontsector->ceilingheight<backsector->ceilingheight
         ? frontsector->ceilingheight : backsector->ceilingheight;
      dcvars.texturemid = dcvars.texturemid - viewz;
   }

   dcvars.texturemid += curline->sidedef->rowoffset;

   if (fixedcolormap)
   {
      dcvars.colormap = fixedcolormap;
      dcvars.nextcolormap = dcvars.colormap; // for filtering -- POPE
   }

   patch = R_CacheTextureCompositePatchNum(texnum);

   /* draw the columns */

   for (dcvars.x = x1 ; dcvars.x <= x2 ; dcvars.x++, spryscale += rw_scalestep)
   {
      if (maskedtexturecol[dcvars.x] == INT_MAX) // dropoff overflow
         continue;

      // calculate texture offset - POPE
      angle = (ds->rw_centerangle + xtoviewangle[dcvars.x]) >> ANGLETOFINESHIFT;
      dcvars.texu = ds->rw_offset - FixedMul(finetangent[angle], ds->rw_distance);
      if (drawvars.filterwall == RDRAW_FILTER_LINEAR)
         dcvars.texu -= (FRACUNIT>>1);

      if (!fixedcolormap)
         dcvars.z = spryscale; // for filtering -- POPE
      dcvars.colormap = R_ColourMap(rw_lightlevel,spryscale);
      dcvars.nextcolormap = R_ColourMap(rw_lightlevel+1,spryscale); // for filtering -- POPE

      // killough 3/2/98:
      //
      // This calculation used to overflow and cause crashes in Doom:
      //
      // sprtopscreen = centeryfrac - FixedMul(dcvars.texturemid, spryscale);
      //
      // This code fixes it, by using double-precision intermediate
      // arithmetic and by skipping the drawing of 2s normals whose
      // mapping to screen coordinates is totally out of range:

      {
         int64_t t = ((int64_t) centeryfrac << FRACBITS) -
            (int64_t) dcvars.texturemid * spryscale;
         if (t + (int64_t) textureheight[texnum] * spryscale < 0 ||
               t > (int64_t) MAX_SCREENHEIGHT << FRACBITS*2)
            continue;        // skip if the texture is out of screen's range
         sprtopscreen = (long)(t >> FRACBITS);
      }

      dcvars.iscale = 0xffffffffu / (unsigned) spryscale;

      // killough 1/25/98: here's where Medusa came in, because
      // it implicitly assumed that the column was all one patch.
      // Originally, Doom did not construct complete columns for
      // multipatched textures, so there were no header or trailer
      // bytes in the column referred to below, which explains
      // the Medusa effect. The fix is to construct TRUE columns
      // when forming multipatched textures (see r_data.c).

      // draw the texture
      R_DrawMaskedColumn(
            patch,
            colfunc,
            &dcvars,
            R_GetPatchColumnWrapped(patch, maskedtexturecol[dcvars.x]),
            R_GetPatchColumnWrapped(patch, maskedtexturecol[dcvars.x]-1),
            R_GetPatchColumnWrapped(patch, maskedtexturecol[dcvars.x]+1)
            );

      maskedtexturecol[dcvars.x] = INT_MAX; // dropoff overflow
   }

   R_UnlockTextureCompositePatchNum(texnum);

   curline = NULL; /* cph 2001/11/18 - must clear curline now we're done with it, so R_ColourMap doesn't try using it for other things */
}

//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling textures.
// CALLED: CORE LOOPING ROUTINE.
//

static int didsolidcol; /* True if at least one column was marked solid */

static void R_RenderSegLoop (void)
{
   const rpatch_t *tex_patch;
   draw_column_vars_t dcvars;
   R_DrawColumn_f colfunc = R_GetDrawColumnFunc(RDC_PIPELINE_STANDARD, drawvars.filterwall, drawvars.filterz);
   fixed_t  texturecolumn = 0;   // shut up compiler warning

   R_SetDefaultDrawColumnVars(&dcvars);

   rendered_segs++;

   for ( ; rw_x < rw_stopx ; rw_x++)
   {
      /* mark floor / ceiling areas */
      int yh = bottomfrac>>heightbits;
      int yl = (topfrac+heightunit-1)>>heightbits;

      // no space above wall?
      int bottom,top = ceilingclip[rw_x]+1;

      if (yl < top)
         yl = top;

      if (markceiling)
      {
         bottom = yl-1;

         if (bottom >= floorclip[rw_x])
            bottom = floorclip[rw_x]-1;

         if (top <= bottom)
         {
            ceilingplane->top[rw_x] = top;
            ceilingplane->bottom[rw_x] = bottom;
         }
         // SoM: this should be set here
         ceilingclip[rw_x] = bottom;
      }

      //      yh = bottomfrac>>heightbits;

      bottom = floorclip[rw_x]-1;
      if (yh > bottom)
         yh = bottom;

      if (markfloor)
      {

         top  = yh < ceilingclip[rw_x] ? ceilingclip[rw_x] : yh;

         if (++top <= bottom)
         {
            floorplane->top[rw_x] = top;
            floorplane->bottom[rw_x] = bottom;
         }
         // SoM: This should be set here to prevent overdraw
         floorclip[rw_x] = top;
      }

      // texturecolumn and lighting are independent of wall tiers
      if (segtextured)
      {
         // calculate texture offset
         angle_t angle =(rw_centerangle+xtoviewangle[rw_x])>>ANGLETOFINESHIFT;

         texturecolumn = rw_offset-FixedMul(finetangent[angle],rw_distance);
         if (drawvars.filterwall == RDRAW_FILTER_LINEAR)
            texturecolumn -= (FRACUNIT>>1);
         dcvars.texu = texturecolumn; // for filtering -- POPE
         texturecolumn >>= FRACBITS;

         dcvars.colormap = R_ColourMap(rw_lightlevel,rw_scale);
         dcvars.nextcolormap = R_ColourMap(rw_lightlevel+1,rw_scale); // for filtering -- POPE
         dcvars.z = rw_scale; // for filtering -- POPE

         dcvars.x = rw_x;
         dcvars.iscale = 0xffffffffu / (unsigned)rw_scale;
      }

      // draw the wall tiers
      if (midtexture)
      {

         dcvars.yl = yl;     // single sided line
         dcvars.yh = yh;
         dcvars.texturemid = rw_midtexturemid;
         tex_patch = R_CacheTextureCompositePatchNum(midtexture);
         dcvars.source = R_GetTextureColumn(tex_patch, texturecolumn);
         dcvars.prevsource = R_GetTextureColumn(tex_patch, texturecolumn-1);
         dcvars.nextsource = R_GetTextureColumn(tex_patch, texturecolumn+1);
         dcvars.texheight = midtexheight;
         colfunc (&dcvars);
         R_UnlockTextureCompositePatchNum(midtexture);
         tex_patch = NULL;
         ceilingclip[rw_x] = viewheight;
         floorclip[rw_x] = -1;
      }
      else
      {

         // two sided line
         if (toptexture)
         {
            // top wall
            int mid = pixhigh>>heightbits;
            pixhigh += pixhighstep;

            if (mid >= floorclip[rw_x])
               mid = floorclip[rw_x]-1;

            if (mid >= yl)
            {
               dcvars.yl = yl;
               dcvars.yh = mid;
               dcvars.texturemid = rw_toptexturemid;
               tex_patch = R_CacheTextureCompositePatchNum(toptexture);
               dcvars.source = R_GetTextureColumn(tex_patch,texturecolumn);
               dcvars.prevsource = R_GetTextureColumn(tex_patch,texturecolumn-1);
               dcvars.nextsource = R_GetTextureColumn(tex_patch,texturecolumn+1);
               dcvars.texheight = toptexheight;
               colfunc (&dcvars);
               R_UnlockTextureCompositePatchNum(toptexture);
               tex_patch = NULL;
               ceilingclip[rw_x] = mid;
            }
            else
               ceilingclip[rw_x] = yl-1;
         }
         else  // no top wall
         {

            if (markceiling)
               ceilingclip[rw_x] = yl-1;
         }

         if (bottomtexture)          // bottom wall
         {
            int mid = (pixlow+heightunit-1)>>heightbits;
            pixlow += pixlowstep;

            // no space above wall?
            if (mid <= ceilingclip[rw_x])
               mid = ceilingclip[rw_x]+1;

            if (mid <= yh)
            {
               dcvars.yl = mid;
               dcvars.yh = yh;
               dcvars.texturemid = rw_bottomtexturemid;
               tex_patch = R_CacheTextureCompositePatchNum(bottomtexture);
               dcvars.source = R_GetTextureColumn(tex_patch, texturecolumn);
               dcvars.prevsource = R_GetTextureColumn(tex_patch, texturecolumn-1);
               dcvars.nextsource = R_GetTextureColumn(tex_patch, texturecolumn+1);
               dcvars.texheight = bottomtexheight;
               colfunc (&dcvars);
               R_UnlockTextureCompositePatchNum(bottomtexture);
               tex_patch = NULL;
               floorclip[rw_x] = mid;
            }
            else
               floorclip[rw_x] = yh+1;
         }
         else        // no bottom wall
         {
            if (markfloor)
               floorclip[rw_x] = yh+1;
         }

         // cph - if we completely blocked further sight through this column,
         // add this info to the solid columns array for r_bsp.c
         if ((markceiling || markfloor) &&
               (floorclip[rw_x] <= ceilingclip[rw_x] + 1)) {
            solidcol[rw_x] = 1; didsolidcol = 1;
         }

         // save texturecol for backdrawing of masked mid texture
         if (maskedtexture)
            maskedtexturecol[rw_x] = texturecolumn;
      }

      rw_scale += rw_scalestep;
      topfrac += topstep;
      bottomfrac += bottomstep;
   }
}

// killough 5/2/98: move from r_main.c, made static, simplified

static fixed_t R_PointToDist(fixed_t x, fixed_t y)
{
   fixed_t dx = D_abs(x - viewx);
   fixed_t dy = D_abs(y - viewy);

   if (dy > dx)
   {
      fixed_t t = dx;
      dx = dy;
      dy = t;
   }

   return FixedDiv(dx, finesine[(tantoangle[FixedDiv(dy,dx) >> DBITS]
            + ANG90) >> ANGLETOFINESHIFT]);
}

//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
void R_StoreWallRange(const int start, const int stop)
{
   fixed_t hyp;
   angle_t offsetangle;

   if (ds_p == drawsegs+maxdrawsegs)   // killough 1/98 -- fix 2s line HOM
   {
      unsigned pos    = ds_p - drawsegs; // jff 8/9/98 fix from ZDOOM1.14a
      unsigned newmax = maxdrawsegs ? maxdrawsegs*2 : 128; // killough
      drawsegs        = Z_Realloc(drawsegs,newmax*sizeof(*drawsegs), PU_STATIC, 0);
      ds_p            = drawsegs + pos; // jff 8/9/98 fix from ZDOOM1.14a
      maxdrawsegs     = newmax;
   }

   if(curline->miniseg == FALSE) // figgi -- skip minisegs
      curline->linedef->flags |= ML_MAPPED;

   sidedef = curline->sidedef;
   linedef = curline->linedef;

   // mark the segment as visible for auto map
   linedef->flags |= ML_MAPPED;

   // calculate rw_distance for scale calculation
   rw_normalangle = curline->angle + ANG90;

   offsetangle = rw_normalangle-rw_angle1;

   if (D_abs(offsetangle) > ANG90)
      offsetangle = ANG90;

   hyp = (viewx==curline->v1->x && viewy==curline->v1->y)?
      0 : R_PointToDist (curline->v1->x, curline->v1->y);
   rw_distance = FixedMul(hyp, finecosine[offsetangle>>ANGLETOFINESHIFT]);

   ds_p->x1 = rw_x = start;
   ds_p->x2 = stop;
   ds_p->curline = curline;
   rw_stopx = stop+1;

   {     // killough 1/6/98, 2/1/98: remove limit on openings
      extern int *openings; // dropoff overflow
      extern size_t maxopenings;
      size_t pos = lastopening - openings;
      size_t need = (rw_stopx - start)*4 + pos;
      if (need > maxopenings)
      {
         drawseg_t *ds;                //jff 8/9/98 needed for fix from ZDoom
         int *oldopenings = openings; // dropoff overflow
         int *oldlast = lastopening; // dropoff overflow

         do
            maxopenings = maxopenings ? maxopenings*2 : 16384;
         while (need > maxopenings);
         openings       = Z_Realloc(openings, maxopenings * sizeof(*openings), PU_STATIC, 0);
         lastopening    = openings + pos;

         // jff 8/9/98 borrowed fix for openings from ZDOOM1.14
         // [RH] We also need to adjust the openings pointers that
         //    were already stored in drawsegs.
         for (ds = drawsegs; ds < ds_p; ds++)
         {
#define ADJUST(p) if (ds->p + ds->x1 >= oldopenings && ds->p + ds->x1 <= oldlast)\
            ds->p = ds->p - oldopenings + openings;
            ADJUST (maskedtexturecol);
            ADJUST (sprtopclip);
            ADJUST (sprbottomclip);
         }
#undef ADJUST
      }
   }  // killough: end of code to remove limits on openings

   if (r_wiggle_fix)
      R_FixWiggle(frontsector);
   else
   {
      /* reset back to defaults */
      max_rwscale = 64 * FRACUNIT;
      heightbits  = 12;
      heightunit  = (1 << 12);
      invhgtbits  = 4;
   }

   // calculate scale at both ends and step

   ds_p->scale1 = rw_scale =
      R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);

   if (stop > start)
   {
      ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
      ds_p->scalestep = rw_scalestep = (ds_p->scale2-rw_scale) / (stop-start);
   }
   else
   {
      ds_p->scale2 = ds_p->scale1;
      ds_p->scalestep = 0;
   }

   // calculate texture boundaries
   //  and decide if floor / ceiling marks are needed

   worldtop = frontsector->ceilingheight - viewz;
   worldbottom = frontsector->floorheight - viewz;

   midtexture = toptexture = bottomtexture = maskedtexture = 0;
   ds_p->maskedtexturecol = NULL;

   if (!backsector)
   {
      // single sided line
      midtexture = texturetranslation[sidedef->midtexture];
      midtexheight = (linedef->r_flags & RF_MID_TILE) ? 0 : textureheight[midtexture] >> FRACBITS;

      // a single sided line is terminal, so it must mark ends
      markfloor = markceiling = TRUE;

      if (linedef->flags & ML_DONTPEGBOTTOM)
      {         // bottom of texture at bottom
         fixed_t vtop = frontsector->floorheight +
            textureheight[sidedef->midtexture];
         rw_midtexturemid = vtop - viewz;
      }
      else        // top of texture at top
         rw_midtexturemid = worldtop;

      rw_midtexturemid += FixedMod(sidedef->rowoffset, textureheight[midtexture]);

      ds_p->silhouette = SIL_BOTH;
      ds_p->sprtopclip = screenheightarray;
      ds_p->sprbottomclip = negonearray;
      ds_p->bsilheight = INT_MAX;
      ds_p->tsilheight = INT_MIN;
   }
   else      // two sided line
   {
      ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
      ds_p->silhouette = 0;

      if (linedef->r_flags & RF_CLOSED) { /* cph - closed 2S line e.g. door */
         // cph - killough's (outdated) comment follows - this deals with both
         // "automap fixes", his and mine
         // killough 1/17/98: this test is required if the fix
         // for the automap bug (r_bsp.c) is used, or else some
         // sprites will be displayed behind closed doors. That
         // fix prevents lines behind closed doors with dropoffs
         // from being displayed on the automap.

         ds_p->silhouette = SIL_BOTH;
         ds_p->sprbottomclip = negonearray;
         ds_p->bsilheight = INT_MAX;
         ds_p->sprtopclip = screenheightarray;
         ds_p->tsilheight = INT_MIN;

      } else { /* not solid - old code */

         if (frontsector->floorheight > backsector->floorheight)
         {
            ds_p->silhouette = SIL_BOTTOM;
            ds_p->bsilheight = frontsector->floorheight;
         }
         else
            if (backsector->floorheight > viewz)
            {
               ds_p->silhouette = SIL_BOTTOM;
               ds_p->bsilheight = INT_MAX;
            }

         if (frontsector->ceilingheight < backsector->ceilingheight)
         {
            ds_p->silhouette |= SIL_TOP;
            ds_p->tsilheight = frontsector->ceilingheight;
         }
         else
            if (backsector->ceilingheight < viewz)
            {
               ds_p->silhouette |= SIL_TOP;
               ds_p->tsilheight = INT_MIN;
            }
      }

      worldhigh = backsector->ceilingheight - viewz;
      worldlow = backsector->floorheight - viewz;

      // hack to allow height changes in outdoor areas
      if (frontsector->ceilingpic == skyflatnum
            && backsector->ceilingpic == skyflatnum)
         worldtop = worldhigh;

      markfloor = worldlow != worldbottom
         || backsector->floorpic != frontsector->floorpic
         || backsector->lightlevel != frontsector->lightlevel

         // killough 3/7/98: Add checks for (x,y) offsets
         || backsector->floor_xoffs != frontsector->floor_xoffs
         || backsector->floor_yoffs != frontsector->floor_yoffs

         // killough 4/15/98: prevent 2s normals
         // from bleeding through deep water
         || frontsector->heightsec != -1

         // killough 4/17/98: draw floors if different light levels
         || backsector->floorlightsec != frontsector->floorlightsec
         ;

      markceiling = worldhigh != worldtop
         || backsector->ceilingpic != frontsector->ceilingpic
         || backsector->lightlevel != frontsector->lightlevel

         // killough 3/7/98: Add checks for (x,y) offsets
         || backsector->ceiling_xoffs != frontsector->ceiling_xoffs
         || backsector->ceiling_yoffs != frontsector->ceiling_yoffs

         // killough 4/15/98: prevent 2s normals
         // from bleeding through fake ceilings
         || (frontsector->heightsec != -1 &&
               frontsector->ceilingpic!=skyflatnum)

         // killough 4/17/98: draw ceilings if different light levels
         || backsector->ceilinglightsec != frontsector->ceilinglightsec
         ;

      if (backsector->ceilingheight <= frontsector->floorheight
            || backsector->floorheight >= frontsector->ceilingheight)
         markceiling = markfloor = TRUE;   // closed door

      if (worldhigh < worldtop)   // top texture
      {
         toptexture = texturetranslation[sidedef->toptexture];
         toptexheight = (linedef->r_flags & RF_TOP_TILE) ? 0 : textureheight[toptexture] >> FRACBITS;
         rw_toptexturemid = linedef->flags & ML_DONTPEGTOP ? worldtop :
            backsector->ceilingheight+textureheight[sidedef->toptexture]-viewz;
         rw_toptexturemid += FixedMod(sidedef->rowoffset, textureheight[toptexture]);
      }

      if (worldlow > worldbottom) // bottom texture
      {
         bottomtexture = texturetranslation[sidedef->bottomtexture];
         bottomtexheight = (linedef->r_flags & RF_BOT_TILE) ? 0 : textureheight[bottomtexture] >> FRACBITS;
         rw_bottomtexturemid = linedef->flags & ML_DONTPEGBOTTOM ? worldtop :
            worldlow;
         rw_bottomtexturemid += FixedMod(sidedef->rowoffset, textureheight[bottomtexture]);
      }

      // allocate space for masked texture tables
      if (sidedef->midtexture)    // masked midtexture
      {
         maskedtexture = TRUE;
         ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
         lastopening += rw_stopx - rw_x;
      }
   }

   // calculate rw_offset (only needed for textured lines)
   segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

   if (segtextured)
   {
      rw_offset = FixedMul (hyp, -finesine[offsetangle >>ANGLETOFINESHIFT]);

      rw_offset += sidedef->textureoffset + curline->offset;

      rw_centerangle = ANG90 + viewangle - rw_normalangle;

      rw_lightlevel = frontsector->lightlevel;
   }

   // Remember the vars used to determine fractional U texture
   // coords for later - POPE
   ds_p->rw_offset = rw_offset;
   ds_p->rw_distance = rw_distance;
   ds_p->rw_centerangle = rw_centerangle;

   // if a floor / ceiling plane is on the wrong side of the view
   // plane, it is definitely invisible and doesn't need to be marked.

   // killough 3/7/98: add deep water check
   if (frontsector->heightsec == -1)
   {
      if (frontsector->floorheight >= viewz)       // above view plane
         markfloor = FALSE;
      if (frontsector->ceilingheight <= viewz &&
            frontsector->ceilingpic != skyflatnum)   // below view plane
         markceiling = FALSE;
   }

   // calculate incremental stepping values for texture edges
   worldtop >>= invhgtbits;
   worldbottom >>= invhgtbits;

   topstep = -FixedMul (rw_scalestep, worldtop);
   topfrac = (centeryfrac >> invhgtbits) - FixedMul (worldtop, rw_scale);

   bottomstep = -FixedMul (rw_scalestep,worldbottom);
   bottomfrac = (centeryfrac >> invhgtbits) - FixedMul (worldbottom, rw_scale);

   if (backsector)
   {
      worldhigh >>= invhgtbits;
      worldlow >>= invhgtbits;

      if (worldhigh < worldtop)
      {
         pixhigh = (centeryfrac >> invhgtbits) - FixedMul (worldhigh, rw_scale);
         pixhighstep = -FixedMul (rw_scalestep,worldhigh);
      }
      if (worldlow > worldbottom)
      {
         pixlow = (centeryfrac >> invhgtbits) - FixedMul (worldlow, rw_scale);
         pixlowstep = -FixedMul (rw_scalestep,worldlow);
      }
   }

   // render it
   if (markceiling) {
      if (ceilingplane)   // killough 4/11/98: add NULL ptr checks
         ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);
      else
         markceiling = 0;
   }

   if (markfloor) {
      if (floorplane)     // killough 4/11/98: add NULL ptr checks
         /* cph 2003/04/18  - ceilingplane and floorplane might be the same
          * visplane (e.g. if both skies); R_CheckPlane doesn't know about
          * modifications to the plane that might happen in parallel with the check
          * being made, so we have to override it and split them anyway if that is
          * a possibility, otherwise the floor marking would overwrite the ceiling
          * marking, resulting in HOM. */
         if (markceiling && ceilingplane == floorplane)
            floorplane = R_DupPlane (floorplane, rw_x, rw_stopx-1);
         else
            floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);
      else
         markfloor = 0;
   }

   didsolidcol = 0;
   R_RenderSegLoop();

   /* cph - if a column was made solid by this wall, we _must_ save full clipping info */
   if (backsector && didsolidcol) {
      if (!(ds_p->silhouette & SIL_BOTTOM)) {
         ds_p->silhouette |= SIL_BOTTOM;
         ds_p->bsilheight = backsector->floorheight;
      }
      if (!(ds_p->silhouette & SIL_TOP)) {
         ds_p->silhouette |= SIL_TOP;
         ds_p->tsilheight = backsector->ceilingheight;
      }
   }

   // save sprite clipping info
   if ((ds_p->silhouette & SIL_TOP || maskedtexture) && !ds_p->sprtopclip)
   {
      memcpy (lastopening, ceilingclip+start, sizeof(int)*(rw_stopx-start)); // dropoff overflow
      ds_p->sprtopclip = lastopening - start;
      lastopening += rw_stopx - start;
   }
   if ((ds_p->silhouette & SIL_BOTTOM || maskedtexture) && !ds_p->sprbottomclip)
   {
      memcpy (lastopening, floorclip+start, sizeof(int)*(rw_stopx-start)); // dropoff overflow
      ds_p->sprbottomclip = lastopening - start;
      lastopening += rw_stopx - start;
   }
   if (maskedtexture && !(ds_p->silhouette & SIL_TOP))
   {
      ds_p->silhouette |= SIL_TOP;
      ds_p->tsilheight = INT_MIN;
   }
   if (maskedtexture && !(ds_p->silhouette & SIL_BOTTOM))
   {
      ds_p->silhouette |= SIL_BOTTOM;
      ds_p->bsilheight = INT_MAX;
   }
   ds_p++;
}
