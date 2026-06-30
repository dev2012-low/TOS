#pragma once

#include <Kernel/Types.h>

/* GDL - Graphic Driver Layer */

/*
 * ============================================================================
 * Pixel formats
 * ============================================================================
 */
#define GDL_FORMAT_RGB565       0x00000001
#define GDL_FORMAT_RGB888       0x00000002
#define GDL_FORMAT_XRGB8888     0x00000003
#define GDL_FORMAT_ARGB8888     0x00000004
#define GDL_FORMAT_XRGB2101010  0x00000005
#define GDL_FORMAT_YUV420       0x00000010
#define GDL_FORMAT_YUV422       0x00000011
#define GDL_FORMAT_YUV444       0x00000012

/*
 * ============================================================================
 * Connector types
 * ============================================================================
 */
#define GDL_CONNECTOR_UNKNOWN   0
#define GDL_CONNECTOR_VGA       1
#define GDL_CONNECTOR_DVI_I     2
#define GDL_CONNECTOR_DVI_D     3
#define GDL_CONNECTOR_HDMI_A    4
#define GDL_CONNECTOR_HDMI_B    5
#define GDL_CONNECTOR_DISPLAYPORT 6
#define GDL_CONNECTOR_LVDS      7
#define GDL_CONNECTOR_EDP       8
#define GDL_CONNECTOR_VIRTUAL   9

/*
 * ============================================================================
 * Encoder types
 * ============================================================================
 */
#define GDL_ENCODER_NONE        0
#define GDL_ENCODER_TMDS        1
#define GDL_ENCODER_LVDS        2
#define GDL_ENCODER_ANALOG      3
#define GDL_ENCODER_DISPLAYPORT 4

/*
 * ============================================================================
 * Mode flags
 * ============================================================================
 */
#define GDL_MODE_FLAG_INTERLACE  (1 << 0)
#define GDL_MODE_FLAG_DBLSCAN    (1 << 1)
#define GDL_MODE_FLAG_HSYNC_POS  (1 << 2)
#define GDL_MODE_FLAG_HSYNC_NEG  (1 << 3)
#define GDL_MODE_FLAG_VSYNC_POS  (1 << 4)
#define GDL_MODE_FLAG_VSYNC_NEG  (1 << 5)

/*
 * ============================================================================
 * Mode types
 * ============================================================================
 */
#define GDL_MODE_TYPE_DRIVER     0x00000001
#define GDL_MODE_TYPE_PREFERRED  0x00000002
#define GDL_MODE_TYPE_DEFAULT    0x00000004
#define GDL_MODE_TYPE_USERDEF    0x00000008

/*
 * ============================================================================
 * GDS errors
 * ============================================================================
 */
#define GDL_OK                    0
#define GDL_ERR_INVALID_PARAM    -1
#define GDL_ERR_NO_MEMORY        -2
#define GDL_ERR_NOT_FOUND        -3
#define GDL_ERR_NOT_SUPPORTED    -4
#define GDL_ERR_BUSY             -5
#define GDL_ERR_TIMEOUT          -6

/*
 * ============================================================================
 * GEM flags
 * ============================================================================
 */
#define GDL_GEM_FLAG_READ        (1 << 0)
#define GDL_GEM_FLAG_WRITE       (1 << 1)
#define GDL_GEM_FLAG_CPU_MAP     (1 << 2)
#define GDL_GEM_FLAG_SCANOUT     (1 << 3)
