/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Color Conversion Routines
 *
 * Copyright 2010 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 * Copyright 2016 Armin Novak <armin.novak@thincast.com>
 * Copyright 2016 Thincast Technologies GmbH
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <winpr/crt.h>

#include <freerdp/log.h>
#include <freerdp/freerdp.h>
#include <freerdp/primitives.h>

#if defined(WITH_CAIRO)
#include <cairo.h>
#endif

#if defined(WITH_SWSCALE)
#include <libswscale/swscale.h>
#endif

#include "color.h"

#define TAG FREERDP_TAG("color")

BYTE* freerdp_glyph_convert(UINT32 width, UINT32 height, const BYTE* WINPR_RESTRICT data)
{
	/*
	 * converts a 1-bit-per-pixel glyph to a one-byte-per-pixel glyph:
	 * this approach uses a little more memory, but provides faster
	 * means of accessing individual pixels in blitting operations
	 */
	const UINT32 scanline = (width + 7) / 8;
	BYTE* dstData = (BYTE*)winpr_aligned_malloc(1ull * width * height, 16);

	if (!dstData)
		return NULL;

	ZeroMemory(dstData, 1ULL * width * height);
	BYTE* dstp = dstData;

	for (UINT32 y = 0; y < height; y++)
	{
		const BYTE* srcp = &data[1ull * y * scanline];

		for (UINT32 x = 0; x < width; x++)
		{
			if ((*srcp & (0x80 >> (x % 8))) != 0)
				*dstp = 0xFF;

			dstp++;

			if (((x + 1) % 8 == 0) && x != 0)
				srcp++;
		}
	}

	return dstData;
}

BOOL freerdp_image_copy_from_monochrome(BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat,
                                        UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst, UINT32 nWidth,
                                        UINT32 nHeight, const BYTE* WINPR_RESTRICT pSrcData,
                                        UINT32 backColor, UINT32 foreColor,
                                        const gdiPalette* WINPR_RESTRICT palette)
{
	const UINT32 dstBytesPerPixel = FreeRDPGetBytesPerPixel(DstFormat);

	if (!pDstData || !pSrcData || !palette)
		return FALSE;

	if (nDstStep == 0)
		nDstStep = dstBytesPerPixel * nWidth;

	const UINT32 monoStep = (nWidth + 7) / 8;

	for (size_t y = 0; y < nHeight; y++)
	{
		BYTE* pDstLine = &pDstData[((nYDst + y) * nDstStep)];
		UINT32 monoBit = 0x80;
		const BYTE* monoBits = &pSrcData[monoStep * y];

		for (size_t x = 0; x < nWidth; x++)
		{
			BYTE* pDstPixel = &pDstLine[((nXDst + x) * FreeRDPGetBytesPerPixel(DstFormat))];
			BOOL monoPixel = (*monoBits & monoBit) ? TRUE : FALSE;

			if (!(monoBit >>= 1))
			{
				monoBits++;
				monoBit = 0x80;
			}

			if (monoPixel)
				FreeRDPWriteColor_int(pDstPixel, DstFormat, backColor);
			else
				FreeRDPWriteColor_int(pDstPixel, DstFormat, foreColor);
		}
	}

	return TRUE;
}

static INLINE UINT32 freerdp_image_inverted_pointer_color(UINT32 x, UINT32 y, UINT32 format)
{
#if 1
	/**
	 * Inverted pointer colors (where individual pixels can change their
	 * color to accommodate the background behind them) only seem to be
	 * supported on Windows.
	 * Using a static replacement color for these pixels (e.g. black)
	 * might result in invisible pointers depending on the background.
	 * This function returns either black or white, depending on the
	 * pixel's position.
	 */
	BYTE fill = (x + y) & 1 ? 0x00 : 0xFF;
#else
	BYTE fill = 0x00;
#endif
	return FreeRDPGetColor(format, fill, fill, fill, 0xFF);
}

/*
 * DIB color palettes are arrays of RGBQUAD structs with colors in BGRX format.
 * They are used only by 1, 2, 4, and 8-bit bitmaps.
 */
static void fill_gdi_palette_for_icon(const BYTE* colorTable, UINT16 cbColorTable,
                                      gdiPalette* palette)
{
	WINPR_ASSERT(palette);

	palette->format = PIXEL_FORMAT_BGRX32;
	ZeroMemory(palette->palette, sizeof(palette->palette));

	if (!cbColorTable)
		return;

	if ((cbColorTable % 4 != 0) || (cbColorTable / 4 > 256))
	{
		WLog_WARN(TAG, "weird palette size: %u", cbColorTable);
		return;
	}

	for (UINT16 i = 0; i < cbColorTable / 4; i++)
	{
		palette->palette[i] = FreeRDPReadColor_int(&colorTable[4ULL * i], palette->format);
	}
}

static INLINE UINT32 div_ceil(UINT32 a, UINT32 b)
{
	return (a + (b - 1)) / b;
}

static INLINE UINT32 round_up(UINT32 a, UINT32 b)
{
	return b * div_ceil(a, b);
}

BOOL freerdp_image_copy_from_icon_data(BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat,
                                       UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst, UINT16 nWidth,
                                       UINT16 nHeight, const BYTE* WINPR_RESTRICT bitsColor,
                                       UINT16 cbBitsColor, const BYTE* WINPR_RESTRICT bitsMask,
                                       UINT16 cbBitsMask, const BYTE* WINPR_RESTRICT colorTable,
                                       UINT16 cbColorTable, UINT32 bpp)
{
	DWORD format = 0;
	gdiPalette palette;

	if (!pDstData || !bitsColor)
		return FALSE;

	/*
	 * Color formats used by icons are DIB bitmap formats (2-bit format
	 * is not used by MS-RDPERP). Note that 16-bit is RGB555, not RGB565,
	 * and that 32-bit format uses BGRA order.
	 */
	switch (bpp)
	{
		case 1:
		case 4:
			/*
			 * These formats are not supported by freerdp_image_copy().
			 * PIXEL_FORMAT_MONO and PIXEL_FORMAT_A4 are *not* correct
			 * color formats for this. Please fix freerdp_image_copy()
			 * if you came here to fix a broken icon of some weird app
			 * that still uses 1 or 4bpp format in the 21st century.
			 */
			WLog_WARN(TAG, "1bpp and 4bpp icons are not supported");
			return FALSE;

		case 8:
			format = PIXEL_FORMAT_RGB8;
			break;

		case 16:
			format = PIXEL_FORMAT_RGB15;
			break;

		case 24:
			format = PIXEL_FORMAT_RGB24;
			break;

		case 32:
			format = PIXEL_FORMAT_BGRA32;
			break;

		default:
			WLog_WARN(TAG, "invalid icon bpp: %" PRIu32, bpp);
			return FALSE;
	}

	/* Ensure we have enough source data bytes for image copy. */
	if (cbBitsColor < nWidth * nHeight * FreeRDPGetBytesPerPixel(format))
		return FALSE;

	fill_gdi_palette_for_icon(colorTable, cbColorTable, &palette);
	if (!freerdp_image_copy_no_overlap(pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth, nHeight,
	                                   bitsColor, format, 0, 0, 0, &palette, FREERDP_FLIP_VERTICAL))
		return FALSE;

	/* apply alpha mask */
	if (FreeRDPColorHasAlpha(DstFormat) && cbBitsMask)
	{
		BYTE nextBit = 0;
		const BYTE* maskByte = NULL;
		UINT32 stride = 0;
		BYTE r = 0;
		BYTE g = 0;
		BYTE b = 0;
		BYTE* dstBuf = pDstData;
		UINT32 dstBpp = FreeRDPGetBytesPerPixel(DstFormat);

		/*
		 * Each byte encodes 8 adjacent pixels (with LSB padding as needed).
		 * And due to hysterical raisins, stride of DIB bitmaps must be
		 * a multiple of 4 bytes.
		 */
		stride = round_up(div_ceil(nWidth, 8), 4);

		for (UINT32 y = 0; y < nHeight; y++)
		{
			maskByte = &bitsMask[1ULL * stride * (nHeight - 1 - y)];
			nextBit = 0x80;

			for (UINT32 x = 0; x < nWidth; x++)
			{
				UINT32 color = 0;
				BYTE alpha = (*maskByte & nextBit) ? 0x00 : 0xFF;

				/* read color back, add alpha and write it back */
				color = FreeRDPReadColor_int(dstBuf, DstFormat);
				FreeRDPSplitColor(color, DstFormat, &r, &g, &b, NULL, &palette);
				color = FreeRDPGetColor(DstFormat, r, g, b, alpha);
				FreeRDPWriteColor_int(dstBuf, DstFormat, color);

				nextBit >>= 1;
				dstBuf += dstBpp;
				if (!nextBit)
				{
					nextBit = 0x80;
					maskByte++;
				}
			}
		}
	}

	return TRUE;
}

static BOOL freerdp_image_copy_from_pointer_data_1bpp(
    BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat, UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst,
    UINT32 nWidth, UINT32 nHeight, const BYTE* WINPR_RESTRICT xorMask, UINT32 xorMaskLength,
    const BYTE* WINPR_RESTRICT andMask, UINT32 andMaskLength, UINT32 xorBpp)
{
	BOOL vFlip = 0;
	UINT32 xorStep = 0;
	UINT32 andStep = 0;
	UINT32 xorBit = 0;
	UINT32 andBit = 0;
	UINT32 xorPixel = 0;
	UINT32 andPixel = 0;

	vFlip = (xorBpp == 1) ? FALSE : TRUE;
	andStep = (nWidth + 7) / 8;
	andStep += (andStep % 2);

	if (!xorMask || (xorMaskLength == 0))
		return FALSE;
	if (!andMask || (andMaskLength == 0))
		return FALSE;

	xorStep = (nWidth + 7) / 8;
	xorStep += (xorStep % 2);

	if (xorStep * nHeight > xorMaskLength)
		return FALSE;

	if (andStep * nHeight > andMaskLength)
		return FALSE;

	for (size_t y = 0; y < nHeight; y++)
	{
		const BYTE* andBits = NULL;
		const BYTE* xorBits = NULL;
		BYTE* pDstPixel = &pDstData[((nYDst + y) * nDstStep) +
		                            (1ULL * nXDst * FreeRDPGetBytesPerPixel(DstFormat))];
		xorBit = andBit = 0x80;

		if (!vFlip)
		{
			xorBits = &xorMask[xorStep * y];
			andBits = &andMask[andStep * y];
		}
		else
		{
			xorBits = &xorMask[xorStep * (nHeight - y - 1)];
			andBits = &andMask[andStep * (nHeight - y - 1)];
		}

		for (UINT32 x = 0; x < nWidth; x++)
		{
			UINT32 color = 0;
			xorPixel = (*xorBits & xorBit) ? 1 : 0;

			if (!(xorBit >>= 1))
			{
				xorBits++;
				xorBit = 0x80;
			}

			andPixel = (*andBits & andBit) ? 1 : 0;

			if (!(andBit >>= 1))
			{
				andBits++;
				andBit = 0x80;
			}

			if (!andPixel && !xorPixel)
				color = FreeRDPGetColor(DstFormat, 0, 0, 0, 0xFF); /* black */
			else if (!andPixel && xorPixel)
				color = FreeRDPGetColor(DstFormat, 0xFF, 0xFF, 0xFF, 0xFF); /* white */
			else if (andPixel && !xorPixel)
				color = FreeRDPGetColor(DstFormat, 0, 0, 0, 0); /* transparent */
			else if (andPixel && xorPixel)
				color = freerdp_image_inverted_pointer_color(x, y, DstFormat); /* inverted */

			FreeRDPWriteColor_int(pDstPixel, DstFormat, color);
			pDstPixel += FreeRDPGetBytesPerPixel(DstFormat);
		}
	}

	return TRUE;
}

static BOOL freerdp_image_copy_from_pointer_data_xbpp(
    BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat, UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst,
    UINT32 nWidth, UINT32 nHeight, const BYTE* WINPR_RESTRICT xorMask, UINT32 xorMaskLength,
    const BYTE* WINPR_RESTRICT andMask, UINT32 andMaskLength, UINT32 xorBpp,
    const gdiPalette* palette)
{
	BOOL vFlip = 0;
	UINT32 xorStep = 0;
	UINT32 andStep = 0;
	UINT32 andBit = 0;
	UINT32 xorPixel = 0;
	UINT32 andPixel = 0;
	UINT32 dstBitsPerPixel = 0;
	UINT32 xorBytesPerPixel = 0;
	dstBitsPerPixel = FreeRDPGetBitsPerPixel(DstFormat);

	vFlip = (xorBpp == 1) ? FALSE : TRUE;
	andStep = (nWidth + 7) / 8;
	andStep += (andStep % 2);

	if (!xorMask || (xorMaskLength == 0))
		return FALSE;

	xorBytesPerPixel = xorBpp >> 3;
	xorStep = nWidth * xorBytesPerPixel;
	xorStep += (xorStep % 2);

	if (xorBpp == 8 && !palette)
	{
		WLog_ERR(TAG, "null palette in conversion from %" PRIu32 " bpp to %" PRIu32 " bpp", xorBpp,
		         dstBitsPerPixel);
		return FALSE;
	}

	if (xorStep * nHeight > xorMaskLength)
		return FALSE;

	if (andMask)
	{
		if (andStep * nHeight > andMaskLength)
			return FALSE;
	}

	for (size_t y = 0; y < nHeight; y++)
	{
		const BYTE* xorBits = NULL;
		const BYTE* andBits = NULL;
		BYTE* pDstPixel = &pDstData[((nYDst + y) * nDstStep) +
		                            (1ULL * nXDst * FreeRDPGetBytesPerPixel(DstFormat))];
		andBit = 0x80;

		if (!vFlip)
		{
			if (andMask)
				andBits = &andMask[andStep * y];

			xorBits = &xorMask[xorStep * y];
		}
		else
		{
			if (andMask)
				andBits = &andMask[andStep * (nHeight - y - 1)];

			xorBits = &xorMask[xorStep * (nHeight - y - 1)];
		}

		for (UINT32 x = 0; x < nWidth; x++)
		{
			UINT32 pixelFormat = 0;
			UINT32 color = 0;

			if (xorBpp == 32)
			{
				pixelFormat = PIXEL_FORMAT_BGRA32;
				xorPixel = FreeRDPReadColor_int(xorBits, pixelFormat);
			}
			else if (xorBpp == 16)
			{
				pixelFormat = PIXEL_FORMAT_RGB15;
				xorPixel = FreeRDPReadColor_int(xorBits, pixelFormat);
			}
			else if (xorBpp == 8)
			{
				pixelFormat = palette->format;
				xorPixel = palette->palette[xorBits[0]];
			}
			else
			{
				pixelFormat = PIXEL_FORMAT_BGR24;
				xorPixel = FreeRDPReadColor_int(xorBits, pixelFormat);
			}

			xorPixel = FreeRDPConvertColor(xorPixel, pixelFormat, PIXEL_FORMAT_ARGB32, palette);
			xorBits += xorBytesPerPixel;
			andPixel = 0;

			if (andMask)
			{
				andPixel = (*andBits & andBit) ? 1 : 0;

				if (!(andBit >>= 1))
				{
					andBits++;
					andBit = 0x80;
				}
			}

			if (andPixel)
			{
				if (xorPixel == 0xFF000000) /* black -> transparent */
					xorPixel = 0x00000000;
				else if (xorPixel == 0xFFFFFFFF) /* white -> inverted */
					xorPixel = freerdp_image_inverted_pointer_color(x, y, PIXEL_FORMAT_ARGB32);
			}

			color = FreeRDPConvertColor(xorPixel, PIXEL_FORMAT_ARGB32, DstFormat, palette);
			FreeRDPWriteColor_int(pDstPixel, DstFormat, color);
			pDstPixel += FreeRDPGetBytesPerPixel(DstFormat);
		}
	}

	return TRUE;
}

/**
 * Drawing Monochrome Pointers:
 * http://msdn.microsoft.com/en-us/library/windows/hardware/ff556143/
 *
 * Drawing Color Pointers:
 * http://msdn.microsoft.com/en-us/library/windows/hardware/ff556138/
 */

BOOL freerdp_image_copy_from_pointer_data(BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat,
                                          UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst,
                                          UINT32 nWidth, UINT32 nHeight,
                                          const BYTE* WINPR_RESTRICT xorMask, UINT32 xorMaskLength,
                                          const BYTE* WINPR_RESTRICT andMask, UINT32 andMaskLength,
                                          UINT32 xorBpp, const gdiPalette* WINPR_RESTRICT palette)
{
	UINT32 dstBitsPerPixel = 0;
	UINT32 dstBytesPerPixel = 0;
	dstBitsPerPixel = FreeRDPGetBitsPerPixel(DstFormat);
	dstBytesPerPixel = FreeRDPGetBytesPerPixel(DstFormat);

	if (nDstStep <= 0)
		nDstStep = dstBytesPerPixel * nWidth;

	for (UINT32 y = nYDst; y < nHeight; y++)
	{
		BYTE* WINPR_RESTRICT pDstLine = &pDstData[y * nDstStep + nXDst * dstBytesPerPixel];
		memset(pDstLine, 0, 1ull * dstBytesPerPixel * (nWidth - nXDst));
	}

	switch (xorBpp)
	{
		case 1:
			return freerdp_image_copy_from_pointer_data_1bpp(
			    pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth, nHeight, xorMask,
			    xorMaskLength, andMask, andMaskLength, xorBpp);

		case 8:
		case 16:
		case 24:
		case 32:
			return freerdp_image_copy_from_pointer_data_xbpp(
			    pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth, nHeight, xorMask,
			    xorMaskLength, andMask, andMaskLength, xorBpp, palette);

		default:
			WLog_ERR(TAG, "failed to convert from %" PRIu32 " bpp to %" PRIu32 " bpp", xorBpp,
			         dstBitsPerPixel);
			return FALSE;
	}
}

static INLINE BOOL overlapping(const BYTE* pDstData, UINT32 nXDst, UINT32 nYDst, UINT32 nDstStep,
                               UINT32 dstBytesPerPixel, const BYTE* pSrcData, UINT32 nXSrc,
                               UINT32 nYSrc, UINT32 nSrcStep, UINT32 srcBytesPerPixel,
                               UINT32 nWidth, UINT32 nHeight)
{
	const BYTE* pDstStart = &pDstData[1ULL * nXDst * dstBytesPerPixel + 1ULL * nYDst * nDstStep];
	const BYTE* pDstEnd = pDstStart + 1ULL * nHeight * nDstStep;
	const BYTE* pSrcStart = &pSrcData[1ULL * nXSrc * srcBytesPerPixel + 1ULL * nYSrc * nSrcStep];
	const BYTE* pSrcEnd = pSrcStart + 1ULL * nHeight * nSrcStep;

	WINPR_UNUSED(nWidth);

	if ((pDstStart >= pSrcStart) && (pDstStart <= pSrcEnd))
		return TRUE;

	if ((pDstEnd >= pSrcStart) && (pDstEnd <= pSrcEnd))
		return TRUE;

	return FALSE;
}

static INLINE BOOL freerdp_image_copy_bgr24_bgrx32(BYTE* WINPR_RESTRICT pDstData, UINT32 nDstStep,
                                                   UINT32 nXDst, UINT32 nYDst, UINT32 nWidth,
                                                   UINT32 nHeight,
                                                   const BYTE* WINPR_RESTRICT pSrcData,
                                                   UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc,
                                                   SSIZE_T srcVMultiplier, SSIZE_T srcVOffset,
                                                   SSIZE_T dstVMultiplier, SSIZE_T dstVOffset)
{

	const SSIZE_T srcByte = 3;
	const SSIZE_T dstByte = 4;

	for (SSIZE_T y = 0; y < nHeight; y++)
	{
		const BYTE* WINPR_RESTRICT srcLine =
		    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
		BYTE* WINPR_RESTRICT dstLine =
		    &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];

		for (SSIZE_T x = 0; x < nWidth; x++)
		{
			dstLine[(x + nXDst) * dstByte + 0] = srcLine[(x + nXSrc) * srcByte + 0];
			dstLine[(x + nXDst) * dstByte + 1] = srcLine[(x + nXSrc) * srcByte + 1];
			dstLine[(x + nXDst) * dstByte + 2] = srcLine[(x + nXSrc) * srcByte + 2];
		}
	}

	return TRUE;
}

static INLINE BOOL freerdp_image_copy_bgrx32_bgrx32(BYTE* WINPR_RESTRICT pDstData, UINT32 nDstStep,
                                                    UINT32 nXDst, UINT32 nYDst, UINT32 nWidth,
                                                    UINT32 nHeight,
                                                    const BYTE* WINPR_RESTRICT pSrcData,
                                                    UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc,
                                                    SSIZE_T srcVMultiplier, SSIZE_T srcVOffset,
                                                    SSIZE_T dstVMultiplier, SSIZE_T dstVOffset)
{

	const SSIZE_T srcByte = 4;
	const SSIZE_T dstByte = 4;

	for (SSIZE_T y = 0; y < nHeight; y++)
	{
		const BYTE* WINPR_RESTRICT srcLine =
		    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
		BYTE* WINPR_RESTRICT dstLine =
		    &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];

		for (SSIZE_T x = 0; x < nWidth; x++)
		{
			dstLine[(x + nXDst) * dstByte + 0] = srcLine[(x + nXSrc) * srcByte + 0];
			dstLine[(x + nXDst) * dstByte + 1] = srcLine[(x + nXSrc) * srcByte + 1];
			dstLine[(x + nXDst) * dstByte + 2] = srcLine[(x + nXSrc) * srcByte + 2];
		}
	}

	return TRUE;
}

static INLINE BOOL freerdp_image_copy_generic(
    BYTE* WINPR_RESTRICT pDstData, UINT32 DstFormat, UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst,
    UINT32 nWidth, UINT32 nHeight, const BYTE* WINPR_RESTRICT pSrcData, UINT32 SrcFormat,
    UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc, const gdiPalette* WINPR_RESTRICT palette,
    SSIZE_T srcVMultiplier, SSIZE_T srcVOffset, SSIZE_T dstVMultiplier, SSIZE_T dstVOffset)
{

	const SSIZE_T srcByte = 4;
	const SSIZE_T dstByte = 4;

	for (SSIZE_T y = 0; y < nHeight; y++)
	{
		const BYTE* WINPR_RESTRICT srcLine =
		    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
		BYTE* WINPR_RESTRICT dstLine =
		    &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];

		UINT32 color = FreeRDPReadColor_int(&srcLine[nXSrc * srcByte], SrcFormat);
		UINT32 oldColor = color;
		UINT32 dstColor = FreeRDPConvertColor(color, SrcFormat, DstFormat, palette);
		FreeRDPWriteColorIgnoreAlpha_int(&dstLine[nXDst * dstByte], DstFormat, dstColor);
		for (SSIZE_T x = 1; x < nWidth; x++)
		{
			color = FreeRDPReadColor_int(&srcLine[(x + nXSrc) * srcByte], SrcFormat);
			if (color == oldColor)
			{
				FreeRDPWriteColorIgnoreAlpha_int(&dstLine[(x + nXDst) * dstByte], DstFormat,
				                                 dstColor);
			}
			else
			{
				oldColor = color;
				dstColor = FreeRDPConvertColor(color, SrcFormat, DstFormat, palette);
				FreeRDPWriteColorIgnoreAlpha_int(&dstLine[(x + nXDst) * dstByte], DstFormat,
				                                 dstColor);
			}
		}
	}

	return TRUE;
}

static INLINE BOOL freerdp_image_copy_no_overlap_dst_alpha(
    BYTE* WINPR_RESTRICT pDstData, DWORD DstFormat, UINT32 nDstStep, UINT32 nXDst, UINT32 nYDst,
    UINT32 nWidth, UINT32 nHeight, const BYTE* WINPR_RESTRICT pSrcData, DWORD SrcFormat,
    UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc, const gdiPalette* WINPR_RESTRICT palette,
    SSIZE_T srcVMultiplier, SSIZE_T srcVOffset, SSIZE_T dstVMultiplier, SSIZE_T dstVOffset)
{
	WINPR_ASSERT(pDstData);
	WINPR_ASSERT(pSrcData);

	switch (SrcFormat)
	{
		case PIXEL_FORMAT_BGR24:
			switch (DstFormat)
			{
				case PIXEL_FORMAT_BGRX32:
				case PIXEL_FORMAT_BGRA32:
					return freerdp_image_copy_bgr24_bgrx32(
					    pDstData, nDstStep, nXDst, nYDst, nWidth, nHeight, pSrcData, nSrcStep,
					    nXSrc, nYSrc, srcVMultiplier, srcVOffset, dstVMultiplier, dstVOffset);
				default:
					break;
			}
			break;
		case PIXEL_FORMAT_BGRX32:
		case PIXEL_FORMAT_BGRA32:
			switch (DstFormat)
			{
				case PIXEL_FORMAT_BGRX32:
				case PIXEL_FORMAT_BGRA32:
					return freerdp_image_copy_bgrx32_bgrx32(
					    pDstData, nDstStep, nXDst, nYDst, nWidth, nHeight, pSrcData, nSrcStep,
					    nXSrc, nYSrc, srcVMultiplier, srcVOffset, dstVMultiplier, dstVOffset);
				default:
					break;
			}
			break;
		default:
			break;
	}

	return freerdp_image_copy_generic(pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth, nHeight,
	                                  pSrcData, SrcFormat, nSrcStep, nXSrc, nYSrc, palette,
	                                  srcVMultiplier, srcVOffset, dstVMultiplier, dstVOffset);
}

BOOL freerdp_image_copy_overlap(BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep, UINT32 nXDst,
                                UINT32 nYDst, UINT32 nWidth, UINT32 nHeight, const BYTE* pSrcData,
                                DWORD SrcFormat, UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc,
                                const gdiPalette* WINPR_RESTRICT palette, UINT32 flags)
{
	const UINT32 dstByte = FreeRDPGetBytesPerPixel(DstFormat);
	const UINT32 srcByte = FreeRDPGetBytesPerPixel(SrcFormat);
	const UINT32 copyDstWidth = nWidth * dstByte;
	const UINT32 xSrcOffset = nXSrc * srcByte;
	const UINT32 xDstOffset = nXDst * dstByte;
	const BOOL vSrcVFlip = (flags & FREERDP_FLIP_VERTICAL) ? TRUE : FALSE;
	SSIZE_T srcVOffset = 0;
	SSIZE_T srcVMultiplier = 1;
	SSIZE_T dstVOffset = 0;
	SSIZE_T dstVMultiplier = 1;

	WINPR_ASSERT(overlapping(pDstData, nXDst, nYDst, nDstStep, dstByte, pSrcData, nXSrc, nYSrc,
	                         nSrcStep, srcByte, nWidth, nHeight));

	if ((nWidth == 0) || (nHeight == 0))
		return TRUE;

	if ((nHeight > INT32_MAX) || (nWidth > INT32_MAX))
		return FALSE;

	if (!pDstData || !pSrcData)
		return FALSE;

	if (nDstStep == 0)
		nDstStep = nWidth * FreeRDPGetBytesPerPixel(DstFormat);

	if (nSrcStep == 0)
		nSrcStep = nWidth * FreeRDPGetBytesPerPixel(SrcFormat);

	if (vSrcVFlip)
	{
		srcVOffset = (nHeight - 1ll) * nSrcStep;
		srcVMultiplier = -1;
	}

	if (((flags & FREERDP_KEEP_DST_ALPHA) != 0) && FreeRDPColorHasAlpha(DstFormat))
	{
		return freerdp_image_copy_no_overlap_dst_alpha(
		    pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth, nHeight, pSrcData, SrcFormat,
		    nSrcStep, nXSrc, nYSrc, palette, srcVMultiplier, srcVOffset, dstVMultiplier,
		    dstVOffset);
	}
	else if (FreeRDPAreColorFormatsEqualNoAlpha_int(SrcFormat, DstFormat))
	{
		/* Copy down */
		if (nYDst < nYSrc)
		{
			for (SSIZE_T y = 0; y < nHeight; y++)
			{
				const BYTE* srcLine =
				    &pSrcData[(y + nYSrc) * nSrcStep * srcVMultiplier + srcVOffset];
				BYTE* dstLine = &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];
				memcpy(&dstLine[xDstOffset], &srcLine[xSrcOffset], copyDstWidth);
			}
		}
		/* Copy up */
		else if (nYDst > nYSrc)
		{
			for (SSIZE_T y = nHeight - 1; y >= 0; y--)
			{
				const BYTE* srcLine =
				    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
				BYTE* dstLine = &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];
				memcpy(&dstLine[xDstOffset], &srcLine[xSrcOffset], copyDstWidth);
			}
		}
		/* Copy left */
		else if (nXSrc > nXDst)
		{
			for (SSIZE_T y = 0; y < nHeight; y++)
			{
				const BYTE* srcLine =
				    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
				BYTE* dstLine = &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];
				memmove(&dstLine[xDstOffset], &srcLine[xSrcOffset], copyDstWidth);
			}
		}
		/* Copy right */
		else if (nXSrc < nXDst)
		{
			for (SSIZE_T y = nHeight - 1; y >= 0; y--)
			{
				const BYTE* srcLine =
				    &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
				BYTE* dstLine = &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];
				memmove(&dstLine[xDstOffset], &srcLine[xSrcOffset], copyDstWidth);
			}
		}
		/* Source and destination are equal... */
		else
		{
		}
	}
	else
	{
		for (SSIZE_T y = 0; y < nHeight; y++)
		{
			const BYTE* srcLine = &pSrcData[srcVMultiplier * (y + nYSrc) * nSrcStep + srcVOffset];
			BYTE* dstLine = &pDstData[dstVMultiplier * (y + nYDst) * nDstStep + dstVOffset];

			UINT32 color = FreeRDPReadColor_int(&srcLine[1ULL * nXSrc * srcByte], SrcFormat);
			UINT32 oldColor = color;
			UINT32 dstColor = FreeRDPConvertColor(color, SrcFormat, DstFormat, palette);
			FreeRDPWriteColor_int(&dstLine[1ULL * nXDst * dstByte], DstFormat, dstColor);
			for (SSIZE_T x = 1; x < nWidth; x++)
			{
				color = FreeRDPReadColor_int(&srcLine[(x + nXSrc) * srcByte], SrcFormat);
				if (color == oldColor)
				{
					FreeRDPWriteColor_int(&dstLine[(x + nXDst) * dstByte], DstFormat, dstColor);
				}
				else
				{
					oldColor = color;
					dstColor = FreeRDPConvertColor(color, SrcFormat, DstFormat, palette);
					FreeRDPWriteColor_int(&dstLine[(x + nXDst) * dstByte], DstFormat, dstColor);
				}
			}
		}
	}

	return TRUE;
}

BOOL freerdp_image_copy(BYTE* pDstData, DWORD DstFormat, UINT32 nDstStep, UINT32 nXDst,
                        UINT32 nYDst, UINT32 nWidth, UINT32 nHeight, const BYTE* pSrcData,
                        DWORD SrcFormat, UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc,
                        const gdiPalette* WINPR_RESTRICT palette, UINT32 flags)
{
	const UINT32 dstByte = FreeRDPGetBytesPerPixel(DstFormat);
	const UINT32 srcByte = FreeRDPGetBytesPerPixel(SrcFormat);

	if ((nHeight > INT32_MAX) || (nWidth > INT32_MAX))
		return FALSE;

	if (!pDstData || !pSrcData)
		return FALSE;

	if ((nWidth == 0) || (nHeight == 0))
		return TRUE;

	if (nDstStep == 0)
		nDstStep = nWidth * FreeRDPGetBytesPerPixel(DstFormat);

	if (nSrcStep == 0)
		nSrcStep = nWidth * FreeRDPGetBytesPerPixel(SrcFormat);

	const BOOL ovl = overlapping(pDstData, nXDst, nYDst, nDstStep, dstByte, pSrcData, nXSrc, nYSrc,
	                             nSrcStep, srcByte, nWidth, nHeight);
	if (ovl)
		return freerdp_image_copy_overlap(pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth,
		                                  nHeight, pSrcData, SrcFormat, nSrcStep, nXSrc, nYSrc,
		                                  palette, flags);
	return freerdp_image_copy_no_overlap(pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth,
	                                     nHeight, pSrcData, SrcFormat, nSrcStep, nXSrc, nYSrc,
	                                     palette, flags);
}

BOOL freerdp_image_fill(BYTE* WINPR_RESTRICT pDstData, DWORD DstFormat, UINT32 nDstStep,
                        UINT32 nXDst, UINT32 nYDst, UINT32 nWidth, UINT32 nHeight, UINT32 color)
{
	if ((nWidth == 0) || (nHeight == 0))
		return TRUE;
	const UINT32 bpp = FreeRDPGetBytesPerPixel(DstFormat);
	BYTE* WINPR_RESTRICT pFirstDstLine = NULL;
	BYTE* WINPR_RESTRICT pFirstDstLineXOffset = NULL;

	if (nDstStep == 0)
		nDstStep = (nXDst + nWidth) * FreeRDPGetBytesPerPixel(DstFormat);

	pFirstDstLine = &pDstData[1ULL * nYDst * nDstStep];
	pFirstDstLineXOffset = &pFirstDstLine[1ULL * nXDst * bpp];

	for (size_t x = 0; x < nWidth; x++)
	{
		BYTE* pDst = &pFirstDstLine[(x + nXDst) * bpp];
		FreeRDPWriteColor_int(pDst, DstFormat, color);
	}

	for (size_t y = 1; y < nHeight; y++)
	{
		BYTE* pDstLine = &pDstData[(y + nYDst) * nDstStep + 1ULL * nXDst * bpp];
		memcpy(pDstLine, pFirstDstLineXOffset, 1ull * nWidth * bpp);
	}

	return TRUE;
}

#if defined(WITH_SWSCALE)
static int av_format_for_buffer(UINT32 format)
{
	switch (format)
	{
		case PIXEL_FORMAT_ARGB32:
			return AV_PIX_FMT_BGRA;

		case PIXEL_FORMAT_XRGB32:
			return AV_PIX_FMT_BGR0;

		case PIXEL_FORMAT_BGRA32:
			return AV_PIX_FMT_RGBA;

		case PIXEL_FORMAT_BGRX32:
			return AV_PIX_FMT_RGB0;

		default:
			return AV_PIX_FMT_NONE;
	}
}
#endif

BOOL freerdp_image_scale(BYTE* WINPR_RESTRICT pDstData, DWORD DstFormat, UINT32 nDstStep,
                         UINT32 nXDst, UINT32 nYDst, UINT32 nDstWidth, UINT32 nDstHeight,
                         const BYTE* WINPR_RESTRICT pSrcData, DWORD SrcFormat, UINT32 nSrcStep,
                         UINT32 nXSrc, UINT32 nYSrc, UINT32 nSrcWidth, UINT32 nSrcHeight)
{
	BOOL rc = FALSE;

	if (nDstStep == 0)
		nDstStep = nDstWidth * FreeRDPGetBytesPerPixel(DstFormat);

	if (nSrcStep == 0)
		nSrcStep = nSrcWidth * FreeRDPGetBytesPerPixel(SrcFormat);

#if defined(WITH_SWSCALE) || defined(WITH_CAIRO)
	const BYTE* src = &pSrcData[nXSrc * FreeRDPGetBytesPerPixel(SrcFormat) + nYSrc * nSrcStep];
	BYTE* dst = &pDstData[nXDst * FreeRDPGetBytesPerPixel(DstFormat) + nYDst * nDstStep];
#endif

	/* direct copy is much faster than scaling, so check if we can simply copy... */
	if ((nDstWidth == nSrcWidth) && (nDstHeight == nSrcHeight))
	{
		return freerdp_image_copy_no_overlap(pDstData, DstFormat, nDstStep, nXDst, nYDst, nDstWidth,
		                                     nDstHeight, pSrcData, SrcFormat, nSrcStep, nXSrc,
		                                     nYSrc, NULL, FREERDP_FLIP_NONE);
	}
	else
#if defined(WITH_SWSCALE)
	{
		int res = 0;
		struct SwsContext* resize = NULL;
		int srcFormat = av_format_for_buffer(SrcFormat);
		int dstFormat = av_format_for_buffer(DstFormat);
		const int srcStep[1] = { (int)nSrcStep };
		const int dstStep[1] = { (int)nDstStep };

		if ((srcFormat == AV_PIX_FMT_NONE) || (dstFormat == AV_PIX_FMT_NONE))
			return FALSE;

		resize = sws_getContext((int)nSrcWidth, (int)nSrcHeight, srcFormat, (int)nDstWidth,
		                        (int)nDstHeight, dstFormat, SWS_BILINEAR, NULL, NULL, NULL);

		if (!resize)
			goto fail;

		res = sws_scale(resize, &src, srcStep, 0, (int)nSrcHeight, &dst, dstStep);
		rc = (res == ((int)nDstHeight));
	fail:
		sws_freeContext(resize);
	}

#elif defined(WITH_CAIRO)
	{
		const double sx = (double)nDstWidth / (double)nSrcWidth;
		const double sy = (double)nDstHeight / (double)nSrcHeight;
		cairo_t* cairo_context;
		cairo_surface_t *csrc, *cdst;

		if ((nSrcWidth > INT_MAX) || (nSrcHeight > INT_MAX) || (nSrcStep > INT_MAX))
			return FALSE;

		if ((nDstWidth > INT_MAX) || (nDstHeight > INT_MAX) || (nDstStep > INT_MAX))
			return FALSE;

		csrc = cairo_image_surface_create_for_data((void*)src, CAIRO_FORMAT_ARGB32, (int)nSrcWidth,
		                                           (int)nSrcHeight, (int)nSrcStep);
		cdst = cairo_image_surface_create_for_data(dst, CAIRO_FORMAT_ARGB32, (int)nDstWidth,
		                                           (int)nDstHeight, (int)nDstStep);

		if (!csrc || !cdst)
			goto fail;

		cairo_context = cairo_create(cdst);

		if (!cairo_context)
			goto fail2;

		cairo_scale(cairo_context, sx, sy);
		cairo_set_operator(cairo_context, CAIRO_OPERATOR_SOURCE);
		cairo_set_source_surface(cairo_context, csrc, 0, 0);
		cairo_paint(cairo_context);
		rc = TRUE;
	fail2:
		cairo_destroy(cairo_context);
	fail:
		cairo_surface_destroy(csrc);
		cairo_surface_destroy(cdst);
	}
#else
	{
		WLog_WARN(TAG, "SmartScaling requested but compiled without libcairo support!");
	}
#endif
	return rc;
}

DWORD FreeRDPAreColorFormatsEqualNoAlpha(DWORD first, DWORD second)
{
	return FreeRDPAreColorFormatsEqualNoAlpha_int(first, second);
}

const char* FreeRDPGetColorFormatName(UINT32 format)
{
	switch (format)
	{
		/* 32bpp formats */
		case PIXEL_FORMAT_ARGB32:
			return "PIXEL_FORMAT_ARGB32";

		case PIXEL_FORMAT_XRGB32:
			return "PIXEL_FORMAT_XRGB32";

		case PIXEL_FORMAT_ABGR32:
			return "PIXEL_FORMAT_ABGR32";

		case PIXEL_FORMAT_XBGR32:
			return "PIXEL_FORMAT_XBGR32";

		case PIXEL_FORMAT_BGRA32:
			return "PIXEL_FORMAT_BGRA32";

		case PIXEL_FORMAT_BGRX32:
			return "PIXEL_FORMAT_BGRX32";

		case PIXEL_FORMAT_RGBA32:
			return "PIXEL_FORMAT_RGBA32";

		case PIXEL_FORMAT_RGBX32:
			return "PIXEL_FORMAT_RGBX32";

		case PIXEL_FORMAT_BGRX32_DEPTH30:
			return "PIXEL_FORMAT_BGRX32_DEPTH30";

		case PIXEL_FORMAT_RGBX32_DEPTH30:
			return "PIXEL_FORMAT_RGBX32_DEPTH30";

		/* 24bpp formats */
		case PIXEL_FORMAT_RGB24:
			return "PIXEL_FORMAT_RGB24";

		case PIXEL_FORMAT_BGR24:
			return "PIXEL_FORMAT_BGR24";

		/* 16bpp formats */
		case PIXEL_FORMAT_RGB16:
			return "PIXEL_FORMAT_RGB16";

		case PIXEL_FORMAT_BGR16:
			return "PIXEL_FORMAT_BGR16";

		case PIXEL_FORMAT_ARGB15:
			return "PIXEL_FORMAT_ARGB15";

		case PIXEL_FORMAT_RGB15:
			return "PIXEL_FORMAT_RGB15";

		case PIXEL_FORMAT_ABGR15:
			return "PIXEL_FORMAT_ABGR15";

		case PIXEL_FORMAT_BGR15:
			return "PIXEL_FORMAT_BGR15";

		/* 8bpp formats */
		case PIXEL_FORMAT_RGB8:
			return "PIXEL_FORMAT_RGB8";

		/* 4 bpp formats */
		case PIXEL_FORMAT_A4:
			return "PIXEL_FORMAT_A4";

		/* 1bpp formats */
		case PIXEL_FORMAT_MONO:
			return "PIXEL_FORMAT_MONO";

		default:
			return "UNKNOWN";
	}
}

void FreeRDPSplitColor(UINT32 color, UINT32 format, BYTE* _r, BYTE* _g, BYTE* _b, BYTE* _a,
                       const gdiPalette* palette)
{
	UINT32 tmp = 0;

	switch (format)
	{
		/* 32bpp formats */
		case PIXEL_FORMAT_ARGB32:
			if (_a)
				*_a = (BYTE)(color >> 24);

			if (_r)
				*_r = (BYTE)(color >> 16);

			if (_g)
				*_g = (BYTE)(color >> 8);

			if (_b)
				*_b = (BYTE)color;

			break;

		case PIXEL_FORMAT_XRGB32:
			if (_r)
				*_r = (BYTE)(color >> 16);

			if (_g)
				*_g = (BYTE)(color >> 8);

			if (_b)
				*_b = (BYTE)color;

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_ABGR32:
			if (_a)
				*_a = (BYTE)(color >> 24);

			if (_b)
				*_b = (BYTE)(color >> 16);

			if (_g)
				*_g = (BYTE)(color >> 8);

			if (_r)
				*_r = (BYTE)color;

			break;

		case PIXEL_FORMAT_XBGR32:
			if (_b)
				*_b = (BYTE)(color >> 16);

			if (_g)
				*_g = (BYTE)(color >> 8);

			if (_r)
				*_r = (BYTE)color;

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_RGBA32:
			if (_r)
				*_r = (BYTE)(color >> 24);

			if (_g)
				*_g = (BYTE)(color >> 16);

			if (_b)
				*_b = (BYTE)(color >> 8);

			if (_a)
				*_a = (BYTE)color;

			break;

		case PIXEL_FORMAT_RGBX32:
			if (_r)
				*_r = (BYTE)(color >> 24);

			if (_g)
				*_g = (BYTE)(color >> 16);

			if (_b)
				*_b = (BYTE)(color >> 8);

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_BGRA32:
			if (_b)
				*_b = (BYTE)(color >> 24);

			if (_g)
				*_g = (BYTE)(color >> 16);

			if (_r)
				*_r = (BYTE)(color >> 8);

			if (_a)
				*_a = (BYTE)color;

			break;

		case PIXEL_FORMAT_BGRX32:
			if (_b)
				*_b = (BYTE)(color >> 24);

			if (_g)
				*_g = (BYTE)(color >> 16);

			if (_r)
				*_r = (BYTE)(color >> 8);

			if (_a)
				*_a = 0xFF;

			break;

		/* 24bpp formats */
		case PIXEL_FORMAT_RGB24:
			if (_r)
				*_r = (BYTE)(color >> 16);

			if (_g)
				*_g = (BYTE)(color >> 8);

			if (_b)
				*_b = (BYTE)color;

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_BGR24:
			if (_b)
				*_b = (BYTE)(color >> 16);

			if (_g)
				*_g = (BYTE)(color >> 8);

			if (_r)
				*_r = (BYTE)color;

			if (_a)
				*_a = 0xFF;

			break;

		/* 16bpp formats */
		case PIXEL_FORMAT_RGB16:
			if (_r)
			{
				const UINT32 c = (color >> 11) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_r = (BYTE)(val > 255 ? 255 : val);
			}

			if (_g)
			{
				const UINT32 c = (color >> 5) & 0x3F;
				const UINT32 val = (c << 2) + c / 4 / 2;
				*_g = (BYTE)(val > 255 ? 255 : val);
			}

			if (_b)
			{
				const UINT32 c = (color)&0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_b = (BYTE)(val > 255 ? 255 : val);
			}

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_BGR16:
			if (_r)
			{
				const UINT32 c = (color)&0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_r = (BYTE)(val > 255 ? 255 : val);
			}

			if (_g)
			{
				const UINT32 c = (color >> 5) & 0x3F;
				const UINT32 val = (c << 2) + c / 4 / 2;
				*_g = (BYTE)(val > 255 ? 255 : val);
			}

			if (_b)
			{
				const UINT32 c = (color >> 11) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_b = (BYTE)(val > 255 ? 255 : val);
			}

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_ARGB15:
			if (_r)
			{
				const UINT32 c = (color >> 10) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_r = (BYTE)(val > 255 ? 255 : val);
			}

			if (_g)
			{
				const UINT32 c = (color >> 5) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_g = (BYTE)(val > 255 ? 255 : val);
			}

			if (_b)
			{
				const UINT32 c = (color)&0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_b = (BYTE)(val > 255 ? 255 : val);
			}

			if (_a)
				*_a = color & 0x8000 ? 0xFF : 0x00;

			break;

		case PIXEL_FORMAT_ABGR15:
			if (_r)
			{
				const UINT32 c = (color)&0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_r = (BYTE)(val > 255 ? 255 : val);
			}

			if (_g)
			{
				const UINT32 c = (color >> 5) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_g = (BYTE)(val > 255 ? 255 : val);
			}

			if (_b)
			{
				const UINT32 c = (color >> 10) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_b = (BYTE)(val > 255 ? 255 : val);
			}

			if (_a)
				*_a = color & 0x8000 ? 0xFF : 0x00;

			break;

		/* 15bpp formats */
		case PIXEL_FORMAT_RGB15:
			if (_r)
			{
				const UINT32 c = (color >> 10) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_r = (BYTE)(val > 255 ? 255 : val);
			}

			if (_g)
			{
				const UINT32 c = (color >> 5) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_g = (BYTE)(val > 255 ? 255 : val);
			}

			if (_b)
			{
				const UINT32 c = (color)&0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_b = (BYTE)(val > 255 ? 255 : val);
			}

			if (_a)
				*_a = 0xFF;

			break;

		case PIXEL_FORMAT_BGR15:
			if (_r)
			{
				const UINT32 c = (color)&0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_r = (BYTE)(val > 255 ? 255 : val);
			}

			if (_g)
			{
				const UINT32 c = (color >> 5) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_g = (BYTE)(val > 255 ? 255 : val);
			}

			if (_b)
			{
				const UINT32 c = (color >> 10) & 0x1F;
				const UINT32 val = (c << 3) + c / 4;
				*_b = (BYTE)(val > 255 ? 255 : val);
			}

			if (_a)
				*_a = 0xFF;

			break;

		/* 8bpp formats */
		case PIXEL_FORMAT_RGB8:
			if (color <= 0xFF)
			{
				tmp = palette->palette[color];
				FreeRDPSplitColor(tmp, palette->format, _r, _g, _b, _a, NULL);
			}
			else
			{
				if (_r)
					*_r = 0x00;

				if (_g)
					*_g = 0x00;

				if (_b)
					*_b = 0x00;

				if (_a)
					*_a = 0x00;
			}

			break;

		/* 1bpp formats */
		case PIXEL_FORMAT_MONO:
			if (_r)
				*_r = (color) ? 0xFF : 0x00;

			if (_g)
				*_g = (color) ? 0xFF : 0x00;

			if (_b)
				*_b = (color) ? 0xFF : 0x00;

			if (_a)
				*_a = (color) ? 0xFF : 0x00;

			break;

		/* 4 bpp formats */
		case PIXEL_FORMAT_A4:
		default:
			if (_r)
				*_r = 0x00;

			if (_g)
				*_g = 0x00;

			if (_b)
				*_b = 0x00;

			if (_a)
				*_a = 0x00;

			WLog_ERR(TAG, "Unsupported format %s", FreeRDPGetColorFormatName(format));
			break;
	}
}

BOOL FreeRDPWriteColorIgnoreAlpha(BYTE* WINPR_RESTRICT dst, UINT32 format, UINT32 color)
{
	return FreeRDPWriteColorIgnoreAlpha_int(dst, format, color);
}

BOOL FreeRDPWriteColor(BYTE* WINPR_RESTRICT dst, UINT32 format, UINT32 color)
{
	return FreeRDPWriteColor_int(dst, format, color);
}

UINT32 FreeRDPReadColor(const BYTE* WINPR_RESTRICT src, UINT32 format)
{
	return FreeRDPReadColor_int(src, format);
}

UINT32 FreeRDPGetColor(UINT32 format, BYTE r, BYTE g, BYTE b, BYTE a)
{
	UINT32 _r = r;
	UINT32 _g = g;
	UINT32 _b = b;
	UINT32 _a = a;
	UINT32 t = 0;

	switch (format)
	{
		/* 32bpp formats */
		case PIXEL_FORMAT_ARGB32:
			return (_a << 24) | (_r << 16) | (_g << 8) | _b;

		case PIXEL_FORMAT_XRGB32:
			return (_r << 16) | (_g << 8) | _b;

		case PIXEL_FORMAT_ABGR32:
			return (_a << 24) | (_b << 16) | (_g << 8) | _r;

		case PIXEL_FORMAT_XBGR32:
			return (_b << 16) | (_g << 8) | _r;

		case PIXEL_FORMAT_RGBA32:
			return (_r << 24) | (_g << 16) | (_b << 8) | _a;

		case PIXEL_FORMAT_RGBX32:
			return (_r << 24) | (_g << 16) | (_b << 8) | _a;

		case PIXEL_FORMAT_BGRA32:
			return (_b << 24) | (_g << 16) | (_r << 8) | _a;

		case PIXEL_FORMAT_BGRX32:
			return (_b << 24) | (_g << 16) | (_r << 8) | _a;

		case PIXEL_FORMAT_RGBX32_DEPTH30:
			// TODO: Not tested
			t = (_r << 22) | (_g << 12) | (_b << 2);
			// NOTE: Swapping byte-order because FreeRDPWriteColor written UINT32 in big-endian
			return ((t & 0xff) << 24) | (((t >> 8) & 0xff) << 16) | (((t >> 16) & 0xff) << 8) |
			       (t >> 24);

		case PIXEL_FORMAT_BGRX32_DEPTH30:
			// NOTE: Swapping b and r channel (unknown reason)
			t = (_r << 22) | (_g << 12) | (_b << 2);
			// NOTE: Swapping byte-order because FreeRDPWriteColor written UINT32 in big-endian
			return ((t & 0xff) << 24) | (((t >> 8) & 0xff) << 16) | (((t >> 16) & 0xff) << 8) |
			       (t >> 24);

		/* 24bpp formats */
		case PIXEL_FORMAT_RGB24:
			return (_r << 16) | (_g << 8) | _b;

		case PIXEL_FORMAT_BGR24:
			return (_b << 16) | (_g << 8) | _r;

		/* 16bpp formats */
		case PIXEL_FORMAT_RGB16:
			return (((_r >> 3) & 0x1F) << 11) | (((_g >> 2) & 0x3F) << 5) | ((_b >> 3) & 0x1F);

		case PIXEL_FORMAT_BGR16:
			return (((_b >> 3) & 0x1F) << 11) | (((_g >> 2) & 0x3F) << 5) | ((_r >> 3) & 0x1F);

		case PIXEL_FORMAT_ARGB15:
			return (((_r >> 3) & 0x1F) << 10) | (((_g >> 3) & 0x1F) << 5) | ((_b >> 3) & 0x1F) |
			       (_a ? 0x8000 : 0x0000);

		case PIXEL_FORMAT_ABGR15:
			return (((_b >> 3) & 0x1F) << 10) | (((_g >> 3) & 0x1F) << 5) | ((_r >> 3) & 0x1F) |
			       (_a ? 0x8000 : 0x0000);

		/* 15bpp formats */
		case PIXEL_FORMAT_RGB15:
			return (((_r >> 3) & 0x1F) << 10) | (((_g >> 3) & 0x1F) << 5) | ((_b >> 3) & 0x1F);

		case PIXEL_FORMAT_BGR15:
			return (((_b >> 3) & 0x1F) << 10) | (((_g >> 3) & 0x1F) << 5) | ((_r >> 3) & 0x1F);

		/* 8bpp formats */
		case PIXEL_FORMAT_RGB8:

		/* 4 bpp formats */
		case PIXEL_FORMAT_A4:

		/* 1bpp formats */
		case PIXEL_FORMAT_MONO:
		default:
			WLog_ERR(TAG, "Unsupported format %s", FreeRDPGetColorFormatName(format));
			return 0;
	}
}

BOOL freerdp_image_copy_no_overlap(BYTE* WINPR_RESTRICT pDstData, DWORD DstFormat, UINT32 nDstStep,
                                   UINT32 nXDst, UINT32 nYDst, UINT32 nWidth, UINT32 nHeight,
                                   const BYTE* WINPR_RESTRICT pSrcData, DWORD SrcFormat,
                                   UINT32 nSrcStep, UINT32 nXSrc, UINT32 nYSrc,
                                   const gdiPalette* WINPR_RESTRICT palette, UINT32 flags)
{
	static primitives_t* prims = NULL;
	if (!prims)
		prims = primitives_get();

	WINPR_ASSERT(!overlapping(pDstData, nXDst, nYDst, nDstStep, FreeRDPGetBytesPerPixel(DstFormat),
	                          pSrcData, nXSrc, nYSrc, nSrcStep, FreeRDPGetBytesPerPixel(SrcFormat),
	                          nWidth, nHeight));
	WINPR_ASSERT(prims);
	WINPR_ASSERT(prims->copy_no_overlap);
	return prims->copy_no_overlap(pDstData, DstFormat, nDstStep, nXDst, nYDst, nWidth, nHeight,
	                              pSrcData, SrcFormat, nSrcStep, nXSrc, nYSrc, palette,
	                              flags) == PRIMITIVES_SUCCESS;
}
