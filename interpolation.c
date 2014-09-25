/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  KernelDensity layer implementation and related functions.
 * Author:   Hermes L. Herrera and the MapServer team.
 *
 ******************************************************************************
 * Copyright (c) 2014 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *****************************************************************************/

#include "mapserver.h"
#include <float.h>
#ifdef USE_GDAL

#include "gdal.h"

/******************************************************************************
 * kernel density.
 ******************************************************************************/
void kernelDensity(imageObj *image, float *values, int width, int height, int npoints,
                   processingParams *psz, unsigned char *iValues);
void kernelDensityProcessing(layerObj *layer, processingParams *psz);

/******************************************************************************
 * kernel density.
 ******************************************************************************/
void idw(float *xyz, int width, int height, int npoints,
         processingParams *psz, unsigned char *iValues);
void idwProcessing(layerObj *layer, processingParams *psz);

//---------------------------------------------------------------------------//
int msInterpolationDataset(mapObj *map, imageObj *image, layerObj *interpolation_layer, void **hDSvoid, void **cleanup_ptr) {

    int status, layer_idx, i, nclasses=0, npoints=0, length=0;
    rectObj searchrect;
    shapeObj shape;
    layerObj *layer;
    float *values = NULL,*xyz_values=NULL;
    int im_width = image->width, im_height = image->height;
    double invcellsize = 1.0 / map->cellsize, georadius=0;
    unsigned char *iValues;
    GDALDatasetH hDS;
    processingParams psz;
    int *classgroup = NULL;

    assert(interpolation_layer->connectiontype == MS_KERNELDENSITY ||
           interpolation_layer->connectiontype == MS_IDW);
    *cleanup_ptr = NULL;

    if(!interpolation_layer->connection || !*interpolation_layer->connection) {
        msSetError(MS_MISCERR, "msInterpolationDataset()", "Interpolation layer has no CONNECTION defined");
        return MS_FAILURE;
    }

    if (interpolation_layer->connectiontype == MS_KERNELDENSITY) {
        kernelDensityProcessing(interpolation_layer, &psz);
    } else if(interpolation_layer->connectiontype == MS_IDW) {
        idwProcessing(interpolation_layer, &psz);
    }

    layer_idx = msGetLayerIndex(map, interpolation_layer->connection);
    if(layer_idx == -1) {
        int nLayers, *aLayers;
        aLayers = msGetLayersIndexByGroup(map, interpolation_layer->connection, &nLayers);
        if(!aLayers || !nLayers) {
            msSetError(MS_MISCERR, "Interpolation layer (%s) references unknown layer (%s)", "msInterpolationDataset()",
                       interpolation_layer->name,interpolation_layer->connection);
            return (MS_FAILURE);
        }
        for(i=0; i<nLayers; i++) {
            layer_idx = aLayers[i];
            layer = GET_LAYER(map, layer_idx);
            if(msScaleInBounds(map->scaledenom, layer->minscaledenom, layer->maxscaledenom))
                break;
        }
        free(aLayers);
        if(i == nLayers) {
            msSetError(MS_MISCERR, "Interpolation layer (%s) references no layer for current scale", "msInterpolationDataset()",
                       interpolation_layer->name);
            return (MS_FAILURE);
        }
    } else {
        layer = GET_LAYER(map, layer_idx);
    }
    /* open the linked layer */
    status = msLayerOpen(layer);
    if(status != MS_SUCCESS) return MS_FAILURE;

    status = msLayerWhichItems(layer, MS_FALSE, NULL);
    if(status != MS_SUCCESS) {
        msLayerClose(layer);
        return MS_FAILURE;
    }

    /* identify target shapes */
    if(layer->transform == MS_TRUE) {
        searchrect = map->extent;
        if(psz.expand_searchrect && interpolation_layer->connectiontype == MS_KERNELDENSITY) {
            georadius = psz.radius * map->cellsize;
            searchrect.minx -= georadius;
            searchrect.miny -= georadius;
            searchrect.maxx += georadius;
            searchrect.maxy += georadius;
            im_width += 2 * psz.radius;
            im_height += 2 * psz.radius;
        }
    } else {
        searchrect.minx = searchrect.miny = 0;
        searchrect.maxx = map->width-1;
        searchrect.maxy = map->height-1;
    }

#ifdef USE_PROJ
    layer->project = msProjectionsDiffer(&(layer->projection), &(map->projection));
    if(layer->project)
        msProjectRect(&map->projection, &layer->projection, &searchrect); /* project the searchrect to source coords */
#endif

    status = msLayerWhichShapes(layer, searchrect, MS_FALSE);
    /* nothing to do */
    if(status == MS_SUCCESS) { /* at least one sample may have overlapped */

        if(layer->classgroup && layer->numclasses > 0)
            classgroup = msAllocateValidClassGroups(layer, &nclasses);

        msInitShape(&shape);
        while((status = msLayerNextShape(layer, &shape)) == MS_SUCCESS) {
            int l,p,s,c;
            double weight = 1.0;
            if(!values){ /* defer allocation until we effectively have a feature */
                values = (float*) msSmallCalloc(im_width * im_height, sizeof(float));
                xyz_values = (float*) msSmallCalloc(im_width * im_height, sizeof(float));
            }
#ifdef USE_PROJ
            if(layer->project)
                msProjectShape(&layer->projection, &map->projection, &shape);
#endif

            /* the weight for the sample is set to 1.0 by default. If the
       * layer has some classes defined, we will read the weight from
       * the class->style->size (which can be binded to an attribute)
       */
            if(layer->numclasses > 0) {
                c = msShapeGetClass(layer, map, &shape, classgroup, nclasses);
                if((c == -1) || (layer->class[c]->status == MS_OFF)) {
                    goto nextshape; /* no class matched, skip */
                }
                for (s = 0; s < layer->class[c]->numstyles; s++) {
                    if (msScaleInBounds(map->scaledenom,
                                        layer->class[c]->styles[s]->minscaledenom,
                                        layer->class[c]->styles[s]->maxscaledenom)) {
                        if(layer->class[c]->styles[s]->bindings[MS_STYLE_BINDING_SIZE].index != -1) {
                            weight = atof(shape.values[layer->class[c]->styles[s]->bindings[MS_STYLE_BINDING_SIZE].index]);
                        } else {
                            weight = layer->class[c]->styles[s]->size;
                        }
                        break;
                    }
                }
                if(s == layer->class[c]->numstyles) {
                    /* no style in scale bounds */
                    goto nextshape;
                }
            }
            for(l=0; l<shape.numlines; l++) {
                for(p=0; p<shape.line[l].numpoints; p++) {
                    int x = MS_MAP2IMAGE_XCELL_IC(shape.line[l].point[p].x, map->extent.minx - georadius, invcellsize);
                    int y = MS_MAP2IMAGE_YCELL_IC(shape.line[l].point[p].y, map->extent.maxy + georadius, invcellsize);
                    if(x>=0 && y>=0 && x<im_width && y<im_height) {
                        float *value = values + y * im_width + x;
                        (*value) += weight;
                        xyz_values[length++] = x;
                        xyz_values[length++] = y;
                        xyz_values[length++] = (*value);
                    }
                }
            }

nextshape:
            msFreeShape(&shape);
        }
        //number of layer points.
        npoints = length/3;
    } else if(status != MS_DONE) {
        msLayerClose(layer);
        return MS_FAILURE;
    }

    /* status == MS_DONE */
    msLayerClose(layer);
    status = MS_SUCCESS;

    if(npoints > 0 && psz.expand_searchrect) {
        iValues = msSmallMalloc(image->width*image->height*sizeof(unsigned char));
    } else {
        iValues = msSmallCalloc(1,image->width*image->height*sizeof(unsigned char));
    }

    if(npoints > 0) { /* no use applying the filtering kernel if we have no samples */
        if (interpolation_layer->connectiontype == MS_KERNELDENSITY) {
            kernelDensity(image, values, im_width, im_height, npoints, &psz, iValues);
        } else if(interpolation_layer->connectiontype == MS_IDW) {
            idw(xyz_values, image->width, image->height, npoints, &psz, iValues);
        }
    }

    free(values);
    free(xyz_values);

    {
        char ds_string [1024];
        double adfGeoTransform[6];
        snprintf(ds_string,1024,"MEM:::DATAPOINTER=%p,PIXELS=%u,LINES=%u,BANDS=1,DATATYPE=Byte,PIXELOFFSET=1,LINEOFFSET=%u",
                 iValues,image->width,image->height,image->width);
        hDS = GDALOpenShared( ds_string, GA_ReadOnly );
        if(hDS == NULL) {
            msSetError(MS_MISCERR,"msInterpolationDataset()","failed to create in-memory gdal dataset for interpolated data");
            status = MS_FAILURE;
            free(iValues);
        }
        adfGeoTransform[0] = map->extent.minx - map->cellsize * 0.5; /* top left x */
        adfGeoTransform[1] = map->cellsize;/* w-e pixel resolution */
        adfGeoTransform[2] = 0; /* 0 */
        adfGeoTransform[3] = map->extent.maxy + map->cellsize * 0.5;/* top left y */
        adfGeoTransform[4] = 0; /* 0 */
        adfGeoTransform[5] = -map->cellsize;/* n-s pixel resolution (negative value) */
        GDALSetGeoTransform(hDS,adfGeoTransform);
        *hDSvoid = hDS;
        *cleanup_ptr = (void*)iValues;
    }
    return status;
}
#else


int msInterpolationDataset(mapObj *map, imageObj *image, layerObj *layer, void **hDSvoid, void **cleanup_ptr) {
    msSetError(MS_MISCERR,"msInterpolationDataset()", "KernelDensity layers require GDAL support, however GDAL support is not compiled in this build");
    return MS_FAILURE;
}

#endif

int msCleanupInterpolationDataset(mapObj *map, imageObj *image, layerObj *layer, void *cleanup_ptr) {
    free(cleanup_ptr);
    return MS_SUCCESS;
}
