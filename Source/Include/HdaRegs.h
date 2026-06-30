#pragma once

#include <Kernel/Types.h>

#define HDA_REG_GCAP          0x00
#define HDA_REG_VMIN          0x02
#define HDA_REG_VMAJ          0x03
#define HDA_REG_GCTL          0x08
#define HDA_REG_WAKEEN        0x0C
#define HDA_REG_STATESTS      0x0E
#define HDA_REG_INTCTL        0x20
#define HDA_REG_INTSTS        0x24
#define HDA_REG_WALLCLK       0x30
#define HDA_REG_CORBLBASE     0x40
#define HDA_REG_CORBUBASE     0x44
#define HDA_REG_CORBWP        0x48
#define HDA_REG_CORBRP        0x4A
#define HDA_REG_CORBCTL       0x4C
#define HDA_REG_RIRBLBASE     0x50
#define HDA_REG_RIRBUBASE     0x54
#define HDA_REG_RIRBWP        0x58
#define HDA_REG_RIRBRP        0x5A
#define HDA_REG_RIRBCTL       0x5C
#define HDA_REG_RIRBSTS       0x5D
#define HDA_REG_RINTCNT       0x5E
#define HDA_REG_DPLBASE       0x70

#define HDA_GCTL_CRST         (1u << 0)
#define HDA_GCTL_FCNTRL       (1u << 1)
#define HDA_CORBCTL_RUN       (1u << 1)
#define HDA_CORBCTL_CMEIE     (1u << 0)
#define HDA_RIRBCTL_RUN       (1u << 1)
#define HDA_RIRBCTL_IRQ_EN    (1u << 0)
#define HDA_RIRBSTS_IRQ       (1u << 0)

#define HDA_SD_CTL_OFF        0x00
#define HDA_SD_STS_OFF        0x03
#define HDA_SD_LPIB_OFF       0x04
#define HDA_SD_CBL_OFF        0x08
#define HDA_SD_LVI_OFF        0x0C
#define HDA_SD_FMT_OFF        0x12
#define HDA_SD_BDPL_OFF       0x18
#define HDA_SD_BDPU_OFF       0x1C

#define HDA_SD_CTL_RUN        (1u << 1)
#define HDA_SD_CTL_RESET      (1u << 0)
#define HDA_SD_STS_BCIS       (1u << 2)
#define HDA_SD_STS_FIFOE      (1u << 3)

#define HDA_SD_BASE(N)        (0x80 + (N) * 0x20)

#define HDA_CORB_SIZE         256
#define HDA_RIRB_SIZE         256
#define HDA_BDL_ENTRIES       4
#define HDA_PCM_BUF_SIZE      (4096 * 4)

#define HDA_VERB_GET_PARAM    0xF00
#define HDA_VERB_GET_CONN     0xF02
#define HDA_VERB_SET_POWER    0x705
#define HDA_VERB_SET_PIN      0x707
#define HDA_VERB_SET_STREAM   0x706
#define HDA_VERB_SET_AMP      0x300
#define HDA_VERB_SET_AMP_GAIN 0x390

#define HDA_PARAM_VENDOR      0x00
#define HDA_PARAM_SUBSYS      0x01
#define HDA_PARAM_FGROUP_TYPE 0x05
#define HDA_PARAM_NODE_COUNT  0x04
#define HDA_PARAM_WIDGET_CAP  0x09
#define HDA_PARAM_PCM         0x0A
#define HDA_PARAM_PIN_CAPS    0x0C
#define HDA_PARAM_CONN_LIST   0x0E
#define HDA_PARAM_POWER       0x0F

#define HDA_POWER_D0          0x00

#define HDA_FMT_48K_16_STEREO 0x00440010u

#define HDA_BDL_IOC           (1u << 31)

typedef struct ATTRIBUTE(packed) HdaBdlEntry {
    UINT32 Addr;
    UINT32 AddrU;
    UINT32 Length;
    UINT32 Flags;
} HdaBdlEntry;
