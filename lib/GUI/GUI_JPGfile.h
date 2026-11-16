/*****************************************************************************
* | File      	:   GUI_JPGfile.h
* | Function    :   Decode a JPEG from the SD card straight into the 6-color
*                   frame buffer (scale/center-crop + Floyd-Steinberg dither)
******************************************************************************/
#ifndef __GUI_JPGFILE_H
#define __GUI_JPGFILE_H

#include "DEV_Config.h"

/*
    Decode `path` into the currently selected Paint image (Paint_SetScale(6)).
    Landscape sources go to 800x480, portrait sources to 480x800, using the same
    "cover + center crop" rule as convert.py and the same pixel orientation as
    GUI_ReadBmp_RGB_6Color(), so a JPG and its converted BMP look identical.
    Returns 0 on success, otherwise a TJpgDec JRESULT (or JPG_ERR_*).
*/
int GUI_ReadJpg_RGB_6Color(const char *path);
const char *GUI_JpgErrorStr(int err);

#define JPG_ERR_OPEN    100     /* f_open failed */
#define JPG_ERR_NOMEM   101     /* malloc failed */

#endif
