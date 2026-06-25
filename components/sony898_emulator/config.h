#pragma once

/* ── USB device identity ─────────────────────────────────────────────────── */
#define CFG_USB_VID          0x054Cu
#define CFG_USB_PID          0x0877u
#define CFG_USB_MANUFACTURER "Sony"
#define CFG_USB_PRODUCT      "UP-D898MD_X898MD"
#define CFG_USB_SERIAL       "0000000"

/* ── IEEE1284 Device ID — static fields ─────────────────────────────────── */
#define CFG_DEV_MFG   "Sony"
#define CFG_DEV_MDL   "UP-D898MD_X898MD"
#define CFG_DEV_DES   "Sony UP-D898MD_X898MD"
#define CFG_DEV_CMD   "SPJL-DS,SPDL-DS2"
#define CFG_DEV_SCDIV "0100"
#define CFG_DEV_SCSYV "01010000"
#define CFG_DEV_SCSYS "0000001000010000000000"
#define CFG_DEV_SCMDS "00000500000100000000"
#define CFG_DEV_SCSYI "100005001000050000000000014500"
#define CFG_DEV_SCSVI "000204000204"
#define CFG_DEV_SCMDI "200406"
/* SCSNO is formatted at runtime from the serial number — see sony898_status.c set_scsno() */
#define CFG_DEV_SCCAI "00000000000000"
#define CFG_DEV_SCGSI "01"
#define CFG_DEV_SCQTI "0001"
#define CFG_DEV_SPUQI "0000"

/* ── Error codes (SCMDE / SCMCE / SCSYE) per printer state ──────────────── */
/* Cover open: real UP-D898MD sends both SCMDE=0800 and SCMCE=01 (driver checks OR) */
#define CFG_ERR_COVER_OPEN_SCMDE       0x0800u
#define CFG_ERR_COVER_OPEN_SCMCE       0x01u
#define CFG_ERR_NO_PAPER_SCMDE         0x0A00u
#define CFG_ERR_NO_RIBBON_SCMDE        0x0002u
#define CFG_ERR_NO_MEDIA_SCMDE         0x0300u
#define CFG_ERR_MEDIA_MISMATCH_SCMDE   0x2000u
#define CFG_ERR_SYSTEM_SCMDE           0x0001u
#define CFG_ERR_SYSTEM_SCSYE           0x01u

/* ── SPDL-DS2 protocol offsets and block sizes ───────────────────────────── */
#define CFG_PDL_HEADER_SIZE   290u
#define CFG_PDL_FOOTER_SIZE   7u
#define CFG_PDL_OFF_WIDTH     40u   /* BE uint16, confirmed from Gutenprint source */
#define CFG_PDL_OFF_HEIGHT    42u

/* ── Image buffer limit (must fit in PSRAM) ──────────────────────────────── */
#define CFG_MAX_IMAGE_BYTES   (6u * 1024u * 1024u)

/* ── Timing ──────────────────────────────────────────────────────────────── */
/* Print speed: UP-D898MD prints ~133 lines/sec → 960 lines ≈ 7.2s */
#define CFG_PRINT_SPEED_LPS         133u
#define CFG_JOB_DONE_IDLE_DELAY_MS  2000u
