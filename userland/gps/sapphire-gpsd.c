/*
 * sapphire-gpsd: Qualcomm QMI LOC (service 0x10) multi-GNSS daemon over /dev/redwood-qmiN.
 *
 * Supports concurrent GPS + GLONASS + Galileo + BeiDou (BDS/COMPASS) + QZSS + SBAS + NavIC
 * tracking, skyview telemetry, NMEA 0183 v4.11 sentence synthesis, and JSON/KV state export.
 *
 * Usage:
 *   sapphire-gpsd --test [seconds] [--nmea]   Run interactively, print GNSS fixes & skyview
 *   sapphire-gpsd                             Run continuously, write /var/tmp/sapphire-gps.{fix,json,nmea}
 *                                             and post Darwin notifications com.sapphire.gps.{fix,sv}
 *
 * Message/TLV IDs follow Qualcomm location_service_v02.h (LA.UM.9.12 / SM6225 QMI_LOC v02):
 *   0x0021 QMI_LOC_REG_EVENTS_REQ                  (TLV 0x01: u64 eventRegMask: POS_REPORT | GNSS_SV_INFO)
 *   0x0022 QMI_LOC_START_REQ                       (TLV 0x01: u8 sessionId, 0x10: u32 recurrence,
 *                                                   0x11: u32 accuracy, 0x12: u32 intermed, 0x13: u32 minInterval)
 *   0x0023 QMI_LOC_STOP_REQ                        (TLV 0x01: u8 sessionId)
 *   0x0024 QMI_LOC_EVENT_POSITION_REPORT_IND
 *   0x0025 QMI_LOC_EVENT_GNSS_SV_INFO_IND          (TLV 0x10: svList [28B], TLV 0x11: expandedSvList [29B])
 *   0x0026 QMI_LOC_EVENT_NMEA_IND
 *   0x0038 QMI_LOC_INJECT_UTC_TIME_REQ             (TLV 0x01: u64 timeUtc ms, TLV 0x02: u32 timeUnc ms)
 *   0x003A QMI_LOC_SET_ENGINE_LOCK_REQ             (TLV 0x01: u32 lockType = 1 NONE)
 *   0x003C QMI_LOC_SET_SBAS_CONFIG_REQ             (TLV 0x01: u8 sbasConfig = 1, TLV 0x10: u8 qzssL1s = 1)
 *   0x003E QMI_LOC_SET_NMEA_TYPES_REQ              (TLV 0x01: u32 nmeaSentenceTypeMask)
 *   0x004A QMI_LOC_SET_OPERATION_MODE_REQ          (TLV 0x01: u32 mode = 1 DEFAULT)
 *   0x0088 QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG (TLV 0x10: u64 measReportConfig, 0x11: u64 svPolyConfig)
 *   0x00B8 QMI_LOC_SET_CONSTELLATION_CONTROL_REQ   (TLV 0x01: u8 reset = 0, TLV 0x10: u64 enableMask)
 *   0x00BA QMI_LOC_REGISTER_MASTER_CLIENT_REQ      (TLV 0x01: u32 key = 0xBAABCDEF)
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <notify.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SVC_LOC                                 0x10

/* QMI LOC v02 request / indication message IDs */
#define QMI_LOC_REG_EVENTS_REQ                  0x0021
#define QMI_LOC_START_REQ                       0x0022
#define QMI_LOC_STOP_REQ                        0x0023
#define QMI_LOC_POS_REPORT_IND                  0x0024
#define QMI_LOC_GNSS_SV_INFO_IND                0x0025
#define QMI_LOC_NMEA_IND                        0x0026
#define QMI_LOC_INJECT_UTC_REQ                  0x0038
#define QMI_LOC_SET_ENGINE_LOCK_REQ             0x003A
#define QMI_LOC_SET_SBAS_CONFIG_REQ             0x003C
#define QMI_LOC_SET_NMEA_TYPES_REQ              0x003E
#define QMI_LOC_SET_OPERATION_MODE_REQ          0x004A
#define QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG 0x0088
#define QMI_LOC_SET_CONSTELLATION_CONTROL_REQ   0x00B8
#define QMI_LOC_REGISTER_MASTER_CLIENT_REQ      0x00BA

#define QMI_LOC_MASTER_CLIENT_KEY               0xBAABCDEFu

/* QMI_LOC_REG_EVENTS_REQ (0x0021) eventRegMask bits */
#define QMI_LOC_EVENT_MASK_POS_REPORT           (1ULL << 0) /* 0x00000001 */
#define QMI_LOC_EVENT_MASK_GNSS_SV_INFO         (1ULL << 1) /* 0x00000002 */
#define QMI_LOC_EVENT_MASK_NMEA                 (1ULL << 2) /* 0x00000004 */

/* QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG (0x0088) qmiLocGNSSConstellEnumT_v02 bits */
#define QMI_LOC_REPORT_SYS_GPS                  (1ULL << 0) /* 0x01 */
#define QMI_LOC_REPORT_SYS_GLO                  (1ULL << 1) /* 0x02 */
#define QMI_LOC_REPORT_SYS_BDS                  (1ULL << 2) /* 0x04 */
#define QMI_LOC_REPORT_SYS_GAL                  (1ULL << 3) /* 0x08 */
#define QMI_LOC_REPORT_SYS_QZSS                 (1ULL << 4) /* 0x10 */
#define QMI_LOC_REPORT_SYS_NAVIC                (1ULL << 5) /* 0x20 */
#define QMI_LOC_REPORT_SYS_ALL                  (QMI_LOC_REPORT_SYS_GPS  | \
                                                 QMI_LOC_REPORT_SYS_GLO  | \
                                                 QMI_LOC_REPORT_SYS_BDS  | \
                                                 QMI_LOC_REPORT_SYS_GAL  | \
                                                 QMI_LOC_REPORT_SYS_QZSS | \
                                                 QMI_LOC_REPORT_SYS_NAVIC)

/* QMI_LOC_SET_CONSTELLATION_CONTROL_REQ (0x00B8) qmiLocConstellationMaskT_v02 bits (GPS is mandatory) */
#define QMI_LOC_CTRL_CONSTELL_GLO               (1ULL << 0) /* 0x01 */
#define QMI_LOC_CTRL_CONSTELL_BDS               (1ULL << 1) /* 0x02 */
#define QMI_LOC_CTRL_CONSTELL_QZSS              (1ULL << 2) /* 0x04 */
#define QMI_LOC_CTRL_CONSTELL_GAL               (1ULL << 3) /* 0x08 */
#define QMI_LOC_CTRL_CONSTELL_NAVIC             (1ULL << 4) /* 0x10 */
#define QMI_LOC_CTRL_CONSTELL_ALL               (QMI_LOC_CTRL_CONSTELL_GLO   | \
                                                 QMI_LOC_CTRL_CONSTELL_BDS   | \
                                                 QMI_LOC_CTRL_CONSTELL_QZSS  | \
                                                 QMI_LOC_CTRL_CONSTELL_GAL   | \
                                                 QMI_LOC_CTRL_CONSTELL_NAVIC)

/* QMI_LOC_SET_NMEA_TYPES_REQ (0x003E) mask (excludes deprecated PQGSA 0x8000 / PQGSV 0x10000) */
#define QMI_LOC_NMEA_MASK_GGA                   (1u << 0)
#define QMI_LOC_NMEA_MASK_RMC                   (1u << 1)
#define QMI_LOC_NMEA_MASK_GPGSV                 (1u << 2)
#define QMI_LOC_NMEA_MASK_GPGSA                 (1u << 3)
#define QMI_LOC_NMEA_MASK_VTG                   (1u << 4)
#define QMI_LOC_NMEA_MASK_GLGSV                 (1u << 7)
#define QMI_LOC_NMEA_MASK_GNGSA                 (1u << 8)
#define QMI_LOC_NMEA_MASK_GNGNS                 (1u << 9)
#define QMI_LOC_NMEA_MASK_GAGSV                 (1u << 11)
#define QMI_LOC_NMEA_MASK_GAGSA                 (1u << 12)
#define QMI_LOC_NMEA_MASK_GNGGA                 (1u << 19)
#define QMI_LOC_NMEA_MASK_GNRMC                 (1u << 20)
#define QMI_LOC_NMEA_MASK_GNVTG                 (1u << 21)
#define QMI_LOC_NMEA_MASK_GBGSA                 (1u << 24)
#define QMI_LOC_NMEA_MASK_GBGSV                 (1u << 25)
#define QMI_LOC_NMEA_MASK_GQGSV                 (1u << 28)
#define QMI_LOC_NMEA_MASK_GIGSV                 (1u << 29)
#define QMI_LOC_NMEA_MASK_MULTI_GNSS ( \
	QMI_LOC_NMEA_MASK_GGA   | QMI_LOC_NMEA_MASK_RMC   | QMI_LOC_NMEA_MASK_GPGSV | \
	QMI_LOC_NMEA_MASK_GPGSA | QMI_LOC_NMEA_MASK_VTG   | QMI_LOC_NMEA_MASK_GLGSV | \
	QMI_LOC_NMEA_MASK_GNGSA | QMI_LOC_NMEA_MASK_GNGNS | QMI_LOC_NMEA_MASK_GAGSV | \
	QMI_LOC_NMEA_MASK_GAGSA | QMI_LOC_NMEA_MASK_GNGGA | QMI_LOC_NMEA_MASK_GNRMC | \
	QMI_LOC_NMEA_MASK_GNVTG | QMI_LOC_NMEA_MASK_GBGSA | QMI_LOC_NMEA_MASK_GBGSV | \
	QMI_LOC_NMEA_MASK_GQGSV | QMI_LOC_NMEA_MASK_GIGSV )

/* qmiLocSvSystemEnumT_v02 in QMI_LOC_EVENT_GNSS_SV_INFO_IND (0x0025) */
enum qmi_loc_sv_system {
	QMI_LOC_SV_SYSTEM_GPS     = 1, /* GPS:              SV ID 1..32 */
	QMI_LOC_SV_SYSTEM_GALILEO = 2, /* Galileo:          SV ID 301..336 */
	QMI_LOC_SV_SYSTEM_SBAS    = 3, /* SBAS:             SV ID 120..158, 183..191 */
	QMI_LOC_SV_SYSTEM_COMPASS = 4, /* Compass/BeiDou:   SV ID 201..263 (legacy enum) */
	QMI_LOC_SV_SYSTEM_GLONASS = 5, /* GLONASS:          SV ID 1..32 or 65..96 */
	QMI_LOC_SV_SYSTEM_BDS     = 6, /* BeiDou (BDS):     SV ID 201..263 */
	QMI_LOC_SV_SYSTEM_QZSS    = 7, /* QZSS:             SV ID 193..200 */
	QMI_LOC_SV_SYSTEM_NAVIC   = 8, /* NavIC / IRNSS:    SV ID 401..414 */
};

/* qmiLocSvInfoValidMaskT_v02 bits */
#define QMI_LOC_SV_VALID_SYSTEM         0x00000001u
#define QMI_LOC_SV_VALID_GNSS_SVID      0x00000002u
#define QMI_LOC_SV_VALID_HEALTH_STATUS  0x00000004u
#define QMI_LOC_SV_VALID_PROCESS_STATUS 0x00000008u
#define QMI_LOC_SV_VALID_SVINFO_MASK    0x00000010u
#define QMI_LOC_SV_VALID_ELEVATION      0x00000020u
#define QMI_LOC_SV_VALID_AZIMUTH        0x00000040u
#define QMI_LOC_SV_VALID_SNR            0x00000080u

/* qmiLocSvInfoMaskT_v02 bits */
#define QMI_LOC_SVINFO_HAS_EPHEMERIS    0x01u
#define QMI_LOC_SVINFO_HAS_ALMANAC      0x02u

/* Packed QMI IDL wire sizes for SV info list elements */
#define QMI_SV_INFO_WIRE_LEN            28u /* qmiLocSvInfoStructT_v02 */
#define QMI_EXP_SV_INFO_WIRE_LEN        29u /* qmiLocExpandedSvInfoStructT_v02 (+1B gloFrequency) */

/* Output paths & Darwin notifications */
#define FIX_PATH                        "/var/tmp/sapphire-gps.fix"
#define FIX_TMP_PATH                    "/var/tmp/sapphire-gps.fix.tmp"
#define JSON_PATH                       "/var/tmp/sapphire-gps.json"
#define JSON_TMP_PATH                   "/var/tmp/sapphire-gps.json.tmp"
#define NMEA_PATH                       "/var/tmp/sapphire-gps.nmea"
#define NMEA_TMP_PATH                   "/var/tmp/sapphire-gps.nmea.tmp"
#define N_GPS_FIX                       "com.sapphire.gps.fix"
#define N_GPS_SV                        "com.sapphire.gps.sv"

/* Multi-GNSS capacities (expandedSvList supports up to 176 SVs across 6+ constellations) */
#define MAX_TRACKED_SVS                 192u
#define MAX_USED_SVS                    192u
#define QMI_RX_BUF_SIZE                 16384u
#define NMEA_OUT_BUF_SIZE               16384u
#define JSON_OUT_BUF_SIZE               32768u

enum gnss_sys {
	GNSS_SYS_UNKNOWN = 0,
	GNSS_SYS_GPS     = 1, /* GP, NMEA 4.11 systemId 1 */
	GNSS_SYS_GLONASS = 2, /* GL, NMEA 4.11 systemId 2 */
	GNSS_SYS_GALILEO = 3, /* GA, NMEA 4.11 systemId 3 */
	GNSS_SYS_BEIDOU  = 4, /* GB, NMEA 4.11 systemId 4 */
	GNSS_SYS_QZSS    = 5, /* GQ, NMEA 4.11 systemId 5 */
	GNSS_SYS_NAVIC   = 6, /* GI, NMEA 4.11 systemId 6 */
	GNSS_SYS_SBAS    = 7, /* GP (WAAS/EGNOS/MSAS, PRN 33..64) */
	GNSS_SYS_COUNT   = 8
};

struct tracked_sv {
	uint8_t  sys;        /* enum gnss_sys */
	uint16_t canon_id;   /* Canonical QMI SV ID (1..32, 65..96, 120..191, 193..200, 201..263, 301..336, 401..414) */
	uint16_t nmea_prn;   /* Normalized NMEA PRN (GP:1..32/33..64, GL:65..96, GA:1..36, GB:1..63, GQ:1..10, GI:1..14) */
	uint8_t  glo_freq;   /* GLONASS frequency channel + 8 (1..14) when reported */
	uint8_t  sv_status;  /* 1=IDLE, 2=SEARCH, 3=TRACK */
	bool     healthy;
	bool     has_eph;
	bool     has_alm;
	bool     has_elev;
	bool     has_azim;
	bool     has_snr;
	bool     used;
	float    elev;       /* Degrees 0..90 */
	float    azim;       /* Degrees 0..360 */
	float    snr;        /* C/N0 dB-Hz */
};

struct gnss_state {
	bool     have_fix;
	bool     have_sv_info;
	bool     have_alt_ellip;
	bool     have_alt_msl;
	bool     have_mag_dev;
	bool     altitude_assumed;
	uint32_t session_status;
	uint32_t tech_mask;
	double   lat;
	double   lon;
	float    acc;
	float    vacc;
	float    alt_msl;
	float    alt_ellip;
	float    spd;
	float    hdg;
	float    mag_dev;
	float    pdop;
	float    hdop;
	float    vdop;
	uint64_t ts_ms;

	uint32_t num_used_sv_ids;
	uint16_t used_sv_ids[MAX_USED_SVS];

	uint32_t num_svs;
	struct tracked_sv svs[MAX_TRACKED_SVS];

	uint32_t in_view_total;
	uint32_t used_total;
	uint32_t in_view_by_sys[GNSS_SYS_COUNT];
	uint32_t used_by_sys[GNSS_SYS_COUNT];
};

struct sweet_qmi_hdr { uint32_t node, port; };
struct sweet_qmi_lookup { uint32_t svc, inst, index, node, port, found_inst; };
#define SWEET_QMI_LOOKUP        _IOWR('q', 1, struct sweet_qmi_lookup)

struct qmi_cli { int fd; uint32_t node, port; uint16_t txn; };
struct qmi_msg { uint8_t b[4096]; uint32_t n; };

static volatile sig_atomic_t g_stop = 0;

static void
on_sig(int s)
{
	(void)s;
	g_stop = 1;
}

static inline float
finite_f(float v, float fallback)
{
	return isfinite(v) ? v : fallback;
}

static inline double
finite_d(double v, double fallback)
{
	return isfinite(v) ? v : fallback;
}

static inline int
round_nearest_int(float v)
{
	if (!isfinite(v)) return 0;
	return (v >= 0.0f) ? (int)(v + 0.5f) : (int)(v - 0.5f);
}

static const char *
gnss_sys_talker(enum gnss_sys sys)
{
	switch (sys) {
	case GNSS_SYS_GPS:
	case GNSS_SYS_SBAS:    return "GP";
	case GNSS_SYS_GLONASS: return "GL";
	case GNSS_SYS_GALILEO: return "GA";
	case GNSS_SYS_BEIDOU:  return "GB";
	case GNSS_SYS_QZSS:    return "GQ";
	case GNSS_SYS_NAVIC:   return "GI";
	default:               return "GN";
	}
}

static const char *
gnss_sys_short_name(enum gnss_sys sys)
{
	switch (sys) {
	case GNSS_SYS_GPS:     return "GPS";
	case GNSS_SYS_GLONASS: return "GLO";
	case GNSS_SYS_GALILEO: return "GAL";
	case GNSS_SYS_BEIDOU:  return "BDS";
	case GNSS_SYS_QZSS:    return "QZSS";
	case GNSS_SYS_NAVIC:   return "NAVIC";
	case GNSS_SYS_SBAS:    return "SBAS";
	default:               return "UNK";
	}
}

/*
 * Classify a satellite from QMI LOC (qmiLocSvSystemEnumT_v02, gnssSvId) or from
 * gnssSvUsedList (qmi_sys == 0) into a normalized constellation, canonical QMI SV ID,
 * and NMEA 0183 v4.11 PRN.
 *
 * Disjoint QMI SV ID bands (65..96 GLONASS, 120..158/183..191 SBAS, 193..200 QZSS,
 * 201..263 BeiDou, 301..336 Galileo, 401..414 NavIC) are checked first so both
 * modern BDS=6/QZSS=7/NAVIC=8 and legacy COMPASS=4 encodings resolve unambiguously.
 */
static enum gnss_sys
classify_and_normalize_sv(uint32_t qmi_sys, uint16_t raw_sv_id,
    uint16_t *out_canon_id, uint16_t *out_nmea_prn)
{
	if (raw_sv_id == 0 || raw_sv_id == 255) {
		return GNSS_SYS_UNKNOWN;
	}

	if (raw_sv_id >= 65 && raw_sv_id <= 96) {
		*out_canon_id = raw_sv_id;
		*out_nmea_prn = raw_sv_id;
		return GNSS_SYS_GLONASS;
	}
	if ((raw_sv_id >= 120 && raw_sv_id <= 158) ||
	    (raw_sv_id >= 183 && raw_sv_id <= 191)) {
		*out_canon_id = raw_sv_id;
		*out_nmea_prn = (raw_sv_id >= 120 && raw_sv_id <= 151)
		    ? (uint16_t)(raw_sv_id - 87) : raw_sv_id;
		return GNSS_SYS_SBAS;
	}
	if (raw_sv_id >= 193 && raw_sv_id <= 200) {
		*out_canon_id = raw_sv_id;
		*out_nmea_prn = (uint16_t)(raw_sv_id - 192);
		return GNSS_SYS_QZSS;
	}
	if (raw_sv_id >= 201 && raw_sv_id <= 263) {
		*out_canon_id = raw_sv_id;
		*out_nmea_prn = (uint16_t)(raw_sv_id - 200);
		return GNSS_SYS_BEIDOU;
	}
	if (raw_sv_id >= 301 && raw_sv_id <= 336) {
		*out_canon_id = raw_sv_id;
		*out_nmea_prn = (uint16_t)(raw_sv_id - 300);
		return GNSS_SYS_GALILEO;
	}
	if (raw_sv_id >= 401 && raw_sv_id <= 414) {
		*out_canon_id = raw_sv_id;
		*out_nmea_prn = (uint16_t)(raw_sv_id - 400);
		return GNSS_SYS_NAVIC;
	}

	/* raw_sv_id < 65: disambiguate via qmiLocSvSystemEnumT_v02 (or 0 for gnssSvUsedList GPS 1..32) */
	switch (qmi_sys) {
	case 0:
	case QMI_LOC_SV_SYSTEM_GPS:
		if (raw_sv_id >= 1 && raw_sv_id <= 32) {
			*out_canon_id = raw_sv_id;
			*out_nmea_prn = raw_sv_id;
			return GNSS_SYS_GPS;
		}
		break;
	case QMI_LOC_SV_SYSTEM_GLONASS:
		if (raw_sv_id >= 1 && raw_sv_id <= 32) {
			*out_canon_id = (uint16_t)(raw_sv_id + 64);
			*out_nmea_prn = (uint16_t)(raw_sv_id + 64);
			return GNSS_SYS_GLONASS;
		}
		break;
	case QMI_LOC_SV_SYSTEM_GALILEO:
		if (raw_sv_id >= 1 && raw_sv_id <= 36) {
			*out_canon_id = (uint16_t)(raw_sv_id + 300);
			*out_nmea_prn = raw_sv_id;
			return GNSS_SYS_GALILEO;
		}
		break;
	case QMI_LOC_SV_SYSTEM_COMPASS:
	case QMI_LOC_SV_SYSTEM_BDS:
		if (raw_sv_id >= 1 && raw_sv_id <= 63) {
			*out_canon_id = (uint16_t)(raw_sv_id + 200);
			*out_nmea_prn = raw_sv_id;
			return GNSS_SYS_BEIDOU;
		}
		break;
	case QMI_LOC_SV_SYSTEM_QZSS:
		if (raw_sv_id >= 1 && raw_sv_id <= 8) {
			*out_canon_id = (uint16_t)(raw_sv_id + 192);
			*out_nmea_prn = raw_sv_id;
			return GNSS_SYS_QZSS;
		}
		break;
	case QMI_LOC_SV_SYSTEM_NAVIC:
		if (raw_sv_id >= 1 && raw_sv_id <= 14) {
			*out_canon_id = (uint16_t)(raw_sv_id + 400);
			*out_nmea_prn = raw_sv_id;
			return GNSS_SYS_NAVIC;
		}
		break;
	case QMI_LOC_SV_SYSTEM_SBAS:
		if (raw_sv_id >= 33 && raw_sv_id <= 64) {
			*out_canon_id = (uint16_t)(raw_sv_id + 87);
			*out_nmea_prn = raw_sv_id;
			return GNSS_SYS_SBAS;
		}
		break;
	default:
		break;
	}

	return GNSS_SYS_UNKNOWN;
}

static int
qmi_open_loc(struct qmi_cli *c)
{
	char path[32];
	c->fd = -1;
	for (int i = 0; i < 16; i++) {
		snprintf(path, sizeof(path), "/dev/redwood-qmi%d", i);
		c->fd = open(path, O_RDWR);
		if (c->fd >= 0) break;
		if (errno != EBUSY) return -1;
	}
	if (c->fd < 0) return -1;
	struct sweet_qmi_lookup l = { SVC_LOC, ~0u, 0, 0, 0, 0 };
	if (ioctl(c->fd, SWEET_QMI_LOOKUP, &l) != 0) {
		close(c->fd);
		c->fd = -1;
		return -1;
	}
	c->node = l.node;
	c->port = l.port;
	c->txn = (uint16_t)(((unsigned)getpid() & 0xffu) << 8) | 1u;
	return 0;
}

static void
m_init(struct qmi_msg *m, uint16_t txn, uint16_t id)
{
	m->b[0] = 0x00; /* request */
	memcpy(m->b + 1, &txn, 2);
	memcpy(m->b + 3, &id, 2);
	m->n = 7;
	uint16_t z = 0;
	memcpy(m->b + 5, &z, 2);
}

static void
m_tlv(struct qmi_msg *m, uint8_t type, const void *val, uint16_t len)
{
	if (m->n + 3u + len > sizeof(m->b)) return;
	m->b[m->n] = type;
	memcpy(m->b + m->n + 1, &len, 2);
	memcpy(m->b + m->n + 3, val, len);
	m->n += 3u + len;
	uint16_t total = (uint16_t)(m->n - 7u);
	memcpy(m->b + 5, &total, 2);
}

static int
qmi_send(struct qmi_cli *c, const struct qmi_msg *m)
{
	uint8_t pkt[sizeof(struct sweet_qmi_hdr) + sizeof(m->b)];
	struct sweet_qmi_hdr h = { c->node, c->port };
	memcpy(pkt, &h, sizeof(h));
	memcpy(pkt + sizeof(h), m->b, m->n);
	ssize_t r = write(c->fd, pkt, sizeof(h) + m->n);
	return (r == (ssize_t)(sizeof(h) + m->n)) ? 0 : -1;
}

static const uint8_t *
tlv_find(const uint8_t *qmi, uint32_t qlen, uint8_t want, uint16_t *out_len)
{
	if (qlen < 7u) return NULL;
	uint32_t off = 7u;
	while (off + 3u <= qlen) {
		uint8_t t = qmi[off];
		uint16_t l;
		memcpy(&l, qmi + off + 1, 2);
		if (off + 3u + l > qlen) break;
		if (t == want) {
			if (out_len) *out_len = l;
			return qmi + off + 3u;
		}
		off += 3u + l;
	}
	return NULL;
}

static int
atomic_write_file(const char *tmp_path, const char *final_path, const char *data, size_t len)
{
	unlink(tmp_path);
	int fd = open(tmp_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0644);
	if (fd < 0) return -1;
	struct stat st;
	if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		close(fd);
		unlink(tmp_path);
		return -1;
	}
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w < 0) {
			if (errno == EINTR) continue;
			close(fd);
			unlink(tmp_path);
			return -1;
		}
		if (w == 0) break;
		off += (size_t)w;
	}
	if (close(fd) != 0 || off != len || rename(tmp_path, final_path) != 0) {
		unlink(tmp_path);
		return -1;
	}
	return 0;
}

static void
buf_appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
    __attribute__((format(printf, 4, 5)));

static void
buf_appendf(char *buf, size_t cap, size_t *len, const char *fmt, ...)
{
	if (*len >= cap) return;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *len, cap - *len, fmt, ap);
	va_end(ap);
	if (n <= 0) return;
	if ((size_t)n >= cap - *len) {
		*len = cap - 1;
	} else {
		*len += (size_t)n;
	}
}

static bool
is_canon_sv_used(const struct gnss_state *st, uint16_t canon_id)
{
	for (uint32_t i = 0; i < st->num_used_sv_ids; i++) {
		if (st->used_sv_ids[i] == canon_id) return true;
	}
	return false;
}

static void
recompute_gnss_counts(struct gnss_state *st)
{
	st->in_view_total = 0;
	st->used_total = 0;
	memset(st->in_view_by_sys, 0, sizeof(st->in_view_by_sys));
	memset(st->used_by_sys, 0, sizeof(st->used_by_sys));

	for (uint32_t i = 0; i < st->num_used_sv_ids; i++) {
		uint16_t cid = 0, prn = 0;
		enum gnss_sys sys = classify_and_normalize_sv(0, st->used_sv_ids[i], &cid, &prn);
		if (sys != GNSS_SYS_UNKNOWN && sys < GNSS_SYS_COUNT) {
			st->used_by_sys[sys]++;
			st->used_total++;
		}
	}

	for (uint32_t i = 0; i < st->num_svs; i++) {
		struct tracked_sv *sv = &st->svs[i];
		sv->used = is_canon_sv_used(st, sv->canon_id);
		if (sv->sys < GNSS_SYS_COUNT) {
			st->in_view_by_sys[sv->sys]++;
			st->in_view_total++;
		}
	}

	/* Ensure used <= in_view even before the first 0x0025 GNSS_SV_INFO arrives */
	for (int s = 1; s < GNSS_SYS_COUNT; s++) {
		if (st->in_view_by_sys[s] < st->used_by_sys[s]) {
			uint32_t diff = st->used_by_sys[s] - st->in_view_by_sys[s];
			st->in_view_by_sys[s] = st->used_by_sys[s];
			st->in_view_total += diff;
		}
	}
}

static int
cmp_tracked_sv(const void *a, const void *b)
{
	const struct tracked_sv *sa = (const struct tracked_sv *)a;
	const struct tracked_sv *sb = (const struct tracked_sv *)b;
	if (sa->sys != sb->sys) return (int)sa->sys - (int)sb->sys;
	if (sa->nmea_prn != sb->nmea_prn) return (int)sa->nmea_prn - (int)sb->nmea_prn;
	return (int)sa->canon_id - (int)sb->canon_id;
}

static void
parse_sv_used_tlv(struct gnss_state *st, const uint8_t *p, uint16_t l)
{
	st->num_used_sv_ids = 0;
	if (!p || l < 1u) return;
	uint32_t count = p[0];
	uint32_t off = 1u;
	if (off + count * 2u > l) {
		count = (l - off) / 2u;
	}
	for (uint32_t i = 0; i < count && st->num_used_sv_ids < MAX_USED_SVS; i++) {
		uint16_t raw_id = 0;
		memcpy(&raw_id, p + off + i * 2u, 2);
		uint16_t cid = 0, prn = 0;
		if (classify_and_normalize_sv(0, raw_id, &cid, &prn) == GNSS_SYS_UNKNOWN) {
			continue;
		}
		bool dup = false;
		for (uint32_t k = 0; k < st->num_used_sv_ids; k++) {
			if (st->used_sv_ids[k] == cid) {
				dup = true;
				break;
			}
		}
		if (!dup) {
			st->used_sv_ids[st->num_used_sv_ids++] = cid;
		}
	}
}

static void
parse_pos_report_ind(struct gnss_state *st, const uint8_t *q, uint32_t qlen)
{
	uint16_t l = 0;
	const uint8_t *p;
	uint32_t status = 99;
	double lat = 0.0, lon = 0.0;
	bool have_lat = false, have_lon = false;

	if ((p = tlv_find(q, qlen, 0x01, &l)) && l >= 4u) memcpy(&status, p, 4);
	st->session_status = status;

	if ((p = tlv_find(q, qlen, 0x10, &l)) && l >= 8u) {
		memcpy(&lat, p, 8);
		have_lat = isfinite(lat) && lat >= -90.0 && lat <= 90.0;
	}
	if ((p = tlv_find(q, qlen, 0x11, &l)) && l >= 8u) {
		memcpy(&lon, p, 8);
		have_lon = isfinite(lon) && lon >= -180.0 && lon <= 180.0;
	}

	/* Prefer expandedGnssSvUsedList (TLV 0x36, up to 176 SVs) over gnssSvUsedList (TLV 0x2C) */
	if ((p = tlv_find(q, qlen, 0x36, &l)) && l >= 1u) {
		parse_sv_used_tlv(st, p, l);
	} else if ((p = tlv_find(q, qlen, 0x2C, &l)) && l >= 1u) {
		parse_sv_used_tlv(st, p, l);
	} else if (!(have_lat && have_lon)) {
		st->num_used_sv_ids = 0;
	}

	if ((p = tlv_find(q, qlen, 0x2D, &l)) && l >= 1u) {
		st->altitude_assumed = (p[0] != 0);
	}

	if (have_lat && have_lon) {
		float v = 0.0f;
		uint64_t ts = 0;

		st->have_fix = true;
		st->lat = lat;
		st->lon = lon;
		st->acc = 999.0f;
		st->vacc = 0.0f;
		st->spd = 0.0f;
		st->hdg = 0.0f;
		st->have_alt_ellip = false;
		st->have_alt_msl = false;
		st->have_mag_dev = false;
		st->pdop = 99.9f;
		st->hdop = 99.9f;
		st->vdop = 99.9f;

		if ((p = tlv_find(q, qlen, 0x12, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			st->acc = finite_f(v, 999.0f);
		}
		if ((p = tlv_find(q, qlen, 0x18, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			st->spd = finite_f(v, 0.0f);
		}
		if ((p = tlv_find(q, qlen, 0x1A, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			if (isfinite(v)) {
				st->alt_ellip = v;
				st->have_alt_ellip = true;
			}
		}
		if ((p = tlv_find(q, qlen, 0x1B, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			if (isfinite(v)) {
				st->alt_msl = v;
				st->have_alt_msl = true;
			}
		}
		if (!st->have_alt_msl && st->have_alt_ellip) {
			st->alt_msl = st->alt_ellip;
		} else if (!st->have_alt_msl) {
			st->alt_msl = 0.0f;
		}
		if ((p = tlv_find(q, qlen, 0x1C, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			st->vacc = finite_f(v, 0.0f);
		}
		if ((p = tlv_find(q, qlen, 0x20, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			st->hdg = finite_f(v, 0.0f);
		}
		if ((p = tlv_find(q, qlen, 0x22, &l)) && l >= 4u) {
			memcpy(&v, p, 4);
			if (isfinite(v)) {
				st->mag_dev = v;
				st->have_mag_dev = true;
			}
		}
		if ((p = tlv_find(q, qlen, 0x23, &l)) && l >= 4u) {
			memcpy(&st->tech_mask, p, 4);
		}
		if ((p = tlv_find(q, qlen, 0x33, &l)) && l >= 12u) {
			/* extDOP: PDOP, HDOP, VDOP, GDOP, TDOP */
			float d[3];
			memcpy(d, p, 12);
			st->pdop = finite_f(d[0], 99.9f);
			st->hdop = finite_f(d[1], 99.9f);
			st->vdop = finite_f(d[2], 99.9f);
		} else if ((p = tlv_find(q, qlen, 0x24, &l)) && l >= 12u) {
			/* DOP: PDOP, HDOP, VDOP */
			float d[3];
			memcpy(d, p, 12);
			st->pdop = finite_f(d[0], 99.9f);
			st->hdop = finite_f(d[1], 99.9f);
			st->vdop = finite_f(d[2], 99.9f);
		}
		if ((p = tlv_find(q, qlen, 0x25, &l)) && l >= 8u) {
			memcpy(&ts, p, 8);
			st->ts_ms = ts;
		} else {
			struct timeval tv;
			gettimeofday(&tv, NULL);
			if (tv.tv_sec > 1577836800L) {
				st->ts_ms = (uint64_t)tv.tv_sec * 1000ULL +
				    (uint64_t)(tv.tv_usec / 1000ULL);
			}
		}
	} else if (status != 0 && status != 1) {
		st->have_fix = false;
	}

	recompute_gnss_counts(st);
}

static void
parse_sv_info_ind(struct gnss_state *st, const uint8_t *q, uint32_t qlen)
{
	uint16_t l = 0;
	const uint8_t *p;
	uint32_t elem_sz = 0;

	if ((p = tlv_find(q, qlen, 0x01, &l)) && l >= 1u) {
		st->altitude_assumed = (p[0] != 0);
	}

	/*
	 * Per Qualcomm location_service_v02.h: if expandedSvList (TLV 0x11, up to 176 SVs,
	 * 29 bytes/SV) is reported, svList (TLV 0x10, up to 80 SVs, 28 bytes/SV) is omitted.
	 */
	if ((p = tlv_find(q, qlen, 0x11, &l)) && l >= 1u) {
		elem_sz = QMI_EXP_SV_INFO_WIRE_LEN;
	} else if ((p = tlv_find(q, qlen, 0x10, &l)) && l >= 1u) {
		elem_sz = QMI_SV_INFO_WIRE_LEN;
	} else {
		st->have_sv_info = true;
		st->num_svs = 0;
		recompute_gnss_counts(st);
		return;
	}

	uint32_t count = p[0];
	uint32_t off = 1u;
	if (off + count * elem_sz > l) {
		if (l >= 2u) {
			uint16_t c16 = 0;
			memcpy(&c16, p, 2);
			if (2u + (uint32_t)c16 * elem_sz == l) {
				count = c16;
				off = 2u;
			} else {
				count = (l - off) / elem_sz;
			}
		} else {
			count = 0;
		}
	}

	st->have_sv_info = true;
	st->num_svs = 0;

	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *e = p + off + i * elem_sz;
		uint32_t valid_mask = 0;
		uint32_t qmi_sys = 0;
		uint16_t raw_sv_id = 0;
		uint8_t  health = 0;
		uint32_t sv_status = 0;
		uint8_t  sv_mask = 0;
		float    elev = 0.0f, azim = 0.0f, snr = 0.0f;
		uint8_t  glo_freq = (elem_sz >= QMI_EXP_SV_INFO_WIRE_LEN) ? e[28] : 0;

		memcpy(&valid_mask, e + 0,  4);
		memcpy(&qmi_sys,    e + 4,  4);
		memcpy(&raw_sv_id,  e + 8,  2);
		health = e[10];
		memcpy(&sv_status,  e + 11, 4);
		sv_mask = e[15];
		memcpy(&elev,       e + 16, 4);
		memcpy(&azim,       e + 20, 4);
		memcpy(&snr,        e + 24, 4);

		if ((valid_mask & (QMI_LOC_SV_VALID_SYSTEM | QMI_LOC_SV_VALID_GNSS_SVID)) !=
		    (QMI_LOC_SV_VALID_SYSTEM | QMI_LOC_SV_VALID_GNSS_SVID)) {
			continue;
		}

		uint16_t canon_id = 0, nmea_prn = 0;
		enum gnss_sys sys = classify_and_normalize_sv(qmi_sys, raw_sv_id, &canon_id, &nmea_prn);
		if (sys == GNSS_SYS_UNKNOWN) continue;

		bool has_elev = (valid_mask & QMI_LOC_SV_VALID_ELEVATION) &&
		    isfinite(elev) && elev >= -90.0f && elev <= 90.0f;
		bool has_azim = (valid_mask & QMI_LOC_SV_VALID_AZIMUTH) &&
		    isfinite(azim) && azim >= 0.0f && azim <= 360.0f;
		bool has_snr = (valid_mask & QMI_LOC_SV_VALID_SNR) &&
		    isfinite(snr) && snr >= 0.0f;
		bool healthy = !(valid_mask & QMI_LOC_SV_VALID_HEALTH_STATUS) || (health != 0);
		bool has_eph = (valid_mask & QMI_LOC_SV_VALID_SVINFO_MASK) &&
		    ((sv_mask & QMI_LOC_SVINFO_HAS_EPHEMERIS) != 0);
		bool has_alm = (valid_mask & QMI_LOC_SV_VALID_SVINFO_MASK) &&
		    ((sv_mask & QMI_LOC_SVINFO_HAS_ALMANAC) != 0);
		uint8_t proc_st = (valid_mask & QMI_LOC_SV_VALID_PROCESS_STATUS)
		    ? (uint8_t)(sv_status & 0xffu) : 0;

		/* Merge dual-band entries (e.g. L1 + L5 / E1 + E5a / B1I + B2a) for the same SV */
		int existing = -1;
		for (uint32_t k = 0; k < st->num_svs; k++) {
			if (st->svs[k].sys == (uint8_t)sys && st->svs[k].canon_id == canon_id) {
				existing = (int)k;
				break;
			}
		}
		if (existing >= 0) {
			struct tracked_sv * dst = &st->svs[existing];
			dst->healthy = dst->healthy || healthy;
			dst->has_eph = dst->has_eph || has_eph;
			dst->has_alm = dst->has_alm || has_alm;
			if (proc_st > dst->sv_status) dst->sv_status = proc_st;
			if (glo_freq != 0 && dst->glo_freq == 0) dst->glo_freq = glo_freq;
			if (!dst->has_elev && has_elev) { dst->has_elev = true; dst->elev = elev; }
			if (!dst->has_azim && has_azim) { dst->has_azim = true; dst->azim = azim; }
			if (has_snr && (!dst->has_snr || snr > dst->snr)) {
				dst->has_snr = true;
				dst->snr = snr;
			}
			continue;
		}

		if (st->num_svs >= MAX_TRACKED_SVS) continue;
		struct tracked_sv *sv = &st->svs[st->num_svs++];
		memset(sv, 0, sizeof(*sv));
		sv->sys = (uint8_t)sys;
		sv->canon_id = canon_id;
		sv->nmea_prn = nmea_prn;
		sv->glo_freq = glo_freq;
		sv->sv_status = proc_st;
		sv->healthy = healthy;
		sv->has_eph = has_eph;
		sv->has_alm = has_alm;
		sv->has_elev = has_elev;
		sv->has_azim = has_azim;
		sv->has_snr = has_snr;
		sv->elev = has_elev ? elev : 0.0f;
		sv->azim = has_azim ? azim : 0.0f;
		sv->snr = has_snr ? snr : 0.0f;
	}

	if (st->num_svs > 1u) {
		qsort(st->svs, st->num_svs, sizeof(st->svs[0]), cmp_tracked_sv);
	}
	recompute_gnss_counts(st);
}

static uint8_t
nmea_checksum(const char *body)
{
	uint8_t cs = 0;
	for (const unsigned char *p = (const unsigned char *)body; *p != '\0'; p++) {
		cs ^= *p;
	}
	return cs;
}

static void
nmea_append_sentence(char *out, size_t cap, size_t *len, const char *body)
{
	uint8_t cs = nmea_checksum(body);
	buf_appendf(out, cap, len, "$%s*%02X\r\n", body, (unsigned)cs);
}

/*
 * Convert decimal degrees into NMEA ddmm.mmmmm (lat) or dddmm.mmmmm (lon) using integer
 * 1e-5 arc-minutes so values near 59.999999' roll cleanly into the next degree instead of 60.00000'.
 */
static void
format_nmea_coord(double deg_val, bool is_lon, char *out, size_t out_sz, char *hemi)
{
	if (!isfinite(deg_val)) {
		out[0] = '\0';
		*hemi = '\0';
		return;
	}
	*hemi = (deg_val < 0.0) ? (is_lon ? 'W' : 'S') : (is_lon ? 'E' : 'N');
	double a = (deg_val < 0.0) ? -deg_val : deg_val;
	double max_deg = is_lon ? 180.0 : 90.0;
	if (a > max_deg) a = max_deg;

	uint64_t total_u5min = (uint64_t)(a * 60.0 * 100000.0 + 0.5);
	uint64_t deg = total_u5min / (60ULL * 100000ULL);
	uint64_t rem = total_u5min % (60ULL * 100000ULL);
	uint64_t min_int = rem / 100000ULL;
	uint64_t min_frac = rem % 100000ULL;

	if (is_lon) {
		snprintf(out, out_sz, "%03llu%02llu.%05llu",
		    (unsigned long long)deg,
		    (unsigned long long)min_int,
		    (unsigned long long)min_frac);
	} else {
		snprintf(out, out_sz, "%02llu%02llu.%05llu",
		    (unsigned long long)deg,
		    (unsigned long long)min_int,
		    (unsigned long long)min_frac);
	}
}

static void
format_nmea_utc(uint64_t ts_ms, char *hms_out, size_t hms_sz, char *dmy_out, size_t dmy_sz)
{
	uint64_t use_ms = ts_ms;
	if (use_ms == 0) {
		struct timeval tv;
		gettimeofday(&tv, NULL);
		if (tv.tv_sec > 1577836800L) {
			use_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
		}
	}
	if (use_ms == 0) {
		snprintf(hms_out, hms_sz, "000000.00");
		snprintf(dmy_out, dmy_sz, "010180");
		return;
	}
	time_t sec = (time_t)(use_ms / 1000ULL);
	unsigned csec = (unsigned)((use_ms % 1000ULL) / 10ULL);
	struct tm tmu;
	if (!gmtime_r(&sec, &tmu)) {
		snprintf(hms_out, hms_sz, "000000.00");
		snprintf(dmy_out, dmy_sz, "010180");
		return;
	}
	unsigned hh = (unsigned)(tmu.tm_hour >= 0 ? tmu.tm_hour % 100 : 0);
	unsigned mm = (unsigned)(tmu.tm_min >= 0 ? tmu.tm_min % 100 : 0);
	unsigned ss = (unsigned)(tmu.tm_sec >= 0 ? tmu.tm_sec % 100 : 0);
	unsigned dd = (unsigned)(tmu.tm_mday >= 1 ? tmu.tm_mday % 100 : 1);
	unsigned mo = (unsigned)(tmu.tm_mon >= 0 ? (tmu.tm_mon + 1) % 100 : 1);
	unsigned yy = (unsigned)(((tmu.tm_year % 100) + 100) % 100);
	snprintf(hms_out, hms_sz, "%02u%02u%02u.%02u", hh, mm, ss, csec % 100u);
	snprintf(dmy_out, dmy_sz, "%02u%02u%02u", dd, mo, yy);
}

static void
emit_nmea_gsv_for_talker(char *out, size_t cap, size_t *len, const char *talker,
    const struct tracked_sv * const *list, uint32_t count)
{
	if (count == 0) return;
	uint32_t total_msgs = (count + 3u) / 4u;
	for (uint32_t msg_idx = 0; msg_idx < total_msgs; msg_idx++) {
		char line[160];
		size_t llen = 0;
		buf_appendf(line, sizeof(line), &llen, "%sGSV,%u,%u,%02u",
		    talker, total_msgs, msg_idx + 1u, count);

		uint32_t base = msg_idx * 4u;
		for (uint32_t j = 0; j < 4u && (base + j) < count; j++) {
			const struct tracked_sv *sv = list[base + j];
			if (sv->nmea_prn < 100u) {
				buf_appendf(line, sizeof(line), &llen, ",%02u", (unsigned)sv->nmea_prn);
			} else {
				buf_appendf(line, sizeof(line), &llen, ",%u", (unsigned)sv->nmea_prn);
			}
			if (sv->has_elev) {
				int el = round_nearest_int(sv->elev);
				if (el < -90) el = -90;
				if (el > 90) el = 90;
				if (el >= 0) {
					buf_appendf(line, sizeof(line), &llen, ",%02d", el);
				} else {
					buf_appendf(line, sizeof(line), &llen, ",%d", el);
				}
			} else {
				buf_appendf(line, sizeof(line), &llen, ",");
			}
			if (sv->has_azim) {
				int az = round_nearest_int(sv->azim) % 360;
				if (az < 0) az += 360;
				buf_appendf(line, sizeof(line), &llen, ",%03d", az);
			} else {
				buf_appendf(line, sizeof(line), &llen, ",");
			}
			if (sv->has_snr) {
				int snr_i = round_nearest_int(sv->snr);
				if (snr_i > 99) snr_i = 99;
				if (snr_i > 0) {
					buf_appendf(line, sizeof(line), &llen, ",%02d", snr_i);
				} else {
					buf_appendf(line, sizeof(line), &llen, ",");
				}
			} else {
				buf_appendf(line, sizeof(line), &llen, ",");
			}
		}
		nmea_append_sentence(out, cap, len, line);
	}
}

static size_t
build_nmea_snapshot(const struct gnss_state *st, char *out, size_t cap)
{
	size_t len = 0;
	char hms[24], dmy[16];
	char lat_s[24], lon_s[24];
	char lat_h = '\0', lon_h = '\0';
	char line[200];

	if (cap == 0) return 0;
	out[0] = '\0';
	format_nmea_utc(st->ts_ms, hms, sizeof(hms), dmy, sizeof(dmy));

	if (st->have_fix) {
		format_nmea_coord(st->lat, false, lat_s, sizeof(lat_s), &lat_h);
		format_nmea_coord(st->lon, true,  lon_s, sizeof(lon_s), &lon_h);

		double spd_kts = (double)finite_f(st->spd, 0.0f) * 1.9438444924406;
		float hdg = finite_f(st->hdg, 0.0f);
		if (hdg < 0.0f || hdg >= 360.0f) hdg = 0.0f;

		char mag_s[24] = ",";
		if (st->have_mag_dev && isfinite(st->mag_dev)) {
			float mv = st->mag_dev < 0.0f ? -st->mag_dev : st->mag_dev;
			char mh = st->mag_dev < 0.0f ? 'W' : 'E';
			snprintf(mag_s, sizeof(mag_s), "%.1f,%c", mv, mh);
		}
		bool dgps = (st->used_by_sys[GNSS_SYS_SBAS] > 0);
		snprintf(line, sizeof(line), "GNRMC,%s,A,%s,%c,%s,%c,%.2f,%.2f,%s,%s,%c",
		    hms, lat_s, lat_h, lon_s, lon_h, spd_kts, hdg, dmy, mag_s, dgps ? 'D' : 'A');
		nmea_append_sentence(out, cap, &len, line);

		unsigned gga_sats = st->used_total > 99u ? 99u : st->used_total;
		float hdop = finite_f(st->hdop, 1.0f);
		float alt = finite_f(st->alt_msl, 0.0f);
		float sep = (st->have_alt_ellip && st->have_alt_msl)
		    ? finite_f(st->alt_ellip - st->alt_msl, 0.0f) : 0.0f;
		snprintf(line, sizeof(line), "GNGGA,%s,%s,%c,%s,%c,%u,%02u,%.1f,%.1f,M,%.1f,M,,",
		    hms, lat_s, lat_h, lon_s, lon_h, dgps ? 2u : 1u, gga_sats, hdop, alt, sep);
		nmea_append_sentence(out, cap, &len, line);
	} else {
		snprintf(line, sizeof(line), "GNRMC,%s,V,,,,,,,%s,,,N", hms, dmy);
		nmea_append_sentence(out, cap, &len, line);
		snprintf(line, sizeof(line), "GNGGA,%s,,,,,0,00,99.9,,,,,,", hms);
		nmea_append_sentence(out, cap, &len, line);
	}

	/* NMEA 0183 v4.11 $GNGSA per constellation (systemId 1=GPS, 2=GLO, 3=GAL, 4=BDS, 5=QZSS, 6=NAVIC) */
	unsigned nav_mode = !st->have_fix ? 1u :
	    ((st->used_total < 4u || st->altitude_assumed) ? 2u : 3u);
	float pdop = st->have_fix ? finite_f(st->pdop, 99.9f) : 99.9f;
	float hdop = st->have_fix ? finite_f(st->hdop, 99.9f) : 99.9f;
	float vdop = st->have_fix ? finite_f(st->vdop, 99.9f) : 99.9f;

	static const enum gnss_sys kGsaOrder[] = {
		GNSS_SYS_GPS, GNSS_SYS_GLONASS, GNSS_SYS_GALILEO,
		GNSS_SYS_BEIDOU, GNSS_SYS_QZSS, GNSS_SYS_NAVIC
	};
	for (size_t sidx = 0; sidx < sizeof(kGsaOrder) / sizeof(kGsaOrder[0]); sidx++) {
		enum gnss_sys sys = kGsaOrder[sidx];
		bool always_emit = (sys == GNSS_SYS_GPS || sys == GNSS_SYS_GLONASS ||
		    sys == GNSS_SYS_GALILEO || sys == GNSS_SYS_BEIDOU);
		if (!always_emit && st->in_view_by_sys[sys] == 0 && st->used_by_sys[sys] == 0) {
			continue;
		}
		size_t llen = 0;
		buf_appendf(line, sizeof(line), &llen, "GNGSA,A,%u", nav_mode);
		uint32_t emitted = 0;
		for (uint32_t i = 0; i < st->num_used_sv_ids && emitted < 12u; i++) {
			uint16_t cid = 0, prn = 0;
			enum gnss_sys usys = classify_and_normalize_sv(0, st->used_sv_ids[i], &cid, &prn);
			if (usys == sys) {
				buf_appendf(line, sizeof(line), &llen, ",%02u", (unsigned)prn);
				emitted++;
			}
		}
		for (; emitted < 12u; emitted++) {
			buf_appendf(line, sizeof(line), &llen, ",");
		}
		buf_appendf(line, sizeof(line), &llen, ",%.1f,%.1f,%.1f,%u",
		    pdop, hdop, vdop, (unsigned)sys);
		nmea_append_sentence(out, cap, &len, line);
	}

	/* Per-constellation GSV skyview sentences ($GPGSV, $GLGSV, $GAGSV, $GBGSV, $GQGSV, $GIGSV) */
	const struct tracked_sv *gp_list[MAX_TRACKED_SVS];
	const struct tracked_sv *gl_list[MAX_TRACKED_SVS];
	const struct tracked_sv *ga_list[MAX_TRACKED_SVS];
	const struct tracked_sv *gb_list[MAX_TRACKED_SVS];
	const struct tracked_sv *gq_list[MAX_TRACKED_SVS];
	const struct tracked_sv *gi_list[MAX_TRACKED_SVS];
	uint32_t n_gp = 0, n_gl = 0, n_ga = 0, n_gb = 0, n_gq = 0, n_gi = 0;

	for (uint32_t i = 0; i < st->num_svs; i++) {
		const struct tracked_sv *sv = &st->svs[i];
		switch ((enum gnss_sys)sv->sys) {
		case GNSS_SYS_GPS:
		case GNSS_SYS_SBAS:    gp_list[n_gp++] = sv; break;
		case GNSS_SYS_GLONASS: gl_list[n_gl++] = sv; break;
		case GNSS_SYS_GALILEO: ga_list[n_ga++] = sv; break;
		case GNSS_SYS_BEIDOU:  gb_list[n_gb++] = sv; break;
		case GNSS_SYS_QZSS:    gq_list[n_gq++] = sv; break;
		case GNSS_SYS_NAVIC:   gi_list[n_gi++] = sv; break;
		default: break;
		}
	}

	emit_nmea_gsv_for_talker(out, cap, &len, "GP", gp_list, n_gp);
	emit_nmea_gsv_for_talker(out, cap, &len, "GL", gl_list, n_gl);
	emit_nmea_gsv_for_talker(out, cap, &len, "GA", ga_list, n_ga);
	emit_nmea_gsv_for_talker(out, cap, &len, "GB", gb_list, n_gb);
	emit_nmea_gsv_for_talker(out, cap, &len, "GQ", gq_list, n_gq);
	emit_nmea_gsv_for_talker(out, cap, &len, "GI", gi_list, n_gi);

	return len;
}

static void
format_active_constellations(const struct gnss_state *st, char *out, size_t cap)
{
	size_t len = 0;
	out[0] = '\0';
	for (int s = 1; s < GNSS_SYS_COUNT; s++) {
		if (st->in_view_by_sys[s] > 0 || st->used_by_sys[s] > 0) {
			buf_appendf(out, cap, &len, "%s%s",
			    len > 0 ? "," : "", gnss_sys_short_name((enum gnss_sys)s));
		}
	}
	if (len == 0) {
		snprintf(out, cap, "NONE");
	}
}

static void
write_fix_file(const struct gnss_state *st)
{
	char buf[1024];
	char active[96];
	size_t len = 0;

	format_active_constellations(st, active, sizeof(active));

	/* Keep original 7 key=value lines first for full backward compatibility */
	buf_appendf(buf, sizeof(buf), &len,
	    "lat=%.7f\n"
	    "lon=%.7f\n"
	    "acc=%.2f\n"
	    "alt=%.2f\n"
	    "spd=%.2f\n"
	    "hdg=%.2f\n"
	    "ts=%llu\n"
	    "vacc=%.2f\n"
	    "pdop=%.2f\n"
	    "hdop=%.2f\n"
	    "vdop=%.2f\n"
	    "sats_used=%u\n"
	    "sats_in_view=%u\n"
	    "sats_gps=%u/%u\n"
	    "sats_glo=%u/%u\n"
	    "sats_gal=%u/%u\n"
	    "sats_bds=%u/%u\n"
	    "sats_qzss=%u/%u\n"
	    "sats_navic=%u/%u\n"
	    "sats_sbas=%u/%u\n"
	    "constellations=%s\n",
	    finite_d(st->lat, 0.0),
	    finite_d(st->lon, 0.0),
	    finite_f(st->acc, 999.0f),
	    finite_f(st->alt_msl, 0.0f),
	    finite_f(st->spd, 0.0f),
	    finite_f(st->hdg, 0.0f),
	    (unsigned long long)st->ts_ms,
	    finite_f(st->vacc, 0.0f),
	    finite_f(st->pdop, 99.9f),
	    finite_f(st->hdop, 99.9f),
	    finite_f(st->vdop, 99.9f),
	    st->used_total,
	    st->in_view_total,
	    st->used_by_sys[GNSS_SYS_GPS],     st->in_view_by_sys[GNSS_SYS_GPS],
	    st->used_by_sys[GNSS_SYS_GLONASS], st->in_view_by_sys[GNSS_SYS_GLONASS],
	    st->used_by_sys[GNSS_SYS_GALILEO], st->in_view_by_sys[GNSS_SYS_GALILEO],
	    st->used_by_sys[GNSS_SYS_BEIDOU],  st->in_view_by_sys[GNSS_SYS_BEIDOU],
	    st->used_by_sys[GNSS_SYS_QZSS],    st->in_view_by_sys[GNSS_SYS_QZSS],
	    st->used_by_sys[GNSS_SYS_NAVIC],   st->in_view_by_sys[GNSS_SYS_NAVIC],
	    st->used_by_sys[GNSS_SYS_SBAS],    st->in_view_by_sys[GNSS_SYS_SBAS],
	    active);

	if (atomic_write_file(FIX_TMP_PATH, FIX_PATH, buf, len) == 0) {
		notify_post(N_GPS_FIX);
	}
}

static size_t
build_json_snapshot(const struct gnss_state *st, char *out, size_t cap)
{
	size_t len = 0;
	buf_appendf(out, cap, &len,
	    "{\n"
	    "  \"has_fix\": %s,\n"
	    "  \"session_status\": %u,\n"
	    "  \"lat\": %.7f,\n"
	    "  \"lon\": %.7f,\n"
	    "  \"acc\": %.2f,\n"
	    "  \"vacc\": %.2f,\n"
	    "  \"alt\": %.2f,\n"
	    "  \"spd\": %.2f,\n"
	    "  \"hdg\": %.2f,\n"
	    "  \"pdop\": %.2f,\n"
	    "  \"hdop\": %.2f,\n"
	    "  \"vdop\": %.2f,\n"
	    "  \"ts_ms\": %llu,\n"
	    "  \"sats_used\": %u,\n"
	    "  \"sats_in_view\": %u,\n"
	    "  \"constellations\": {\n"
	    "    \"gps\": {\"used\": %u, \"in_view\": %u},\n"
	    "    \"glonass\": {\"used\": %u, \"in_view\": %u},\n"
	    "    \"galileo\": {\"used\": %u, \"in_view\": %u},\n"
	    "    \"beidou\": {\"used\": %u, \"in_view\": %u},\n"
	    "    \"qzss\": {\"used\": %u, \"in_view\": %u},\n"
	    "    \"navic\": {\"used\": %u, \"in_view\": %u},\n"
	    "    \"sbas\": {\"used\": %u, \"in_view\": %u}\n"
	    "  },\n"
	    "  \"satellites\": [",
	    st->have_fix ? "true" : "false",
	    st->session_status,
	    finite_d(st->lat, 0.0),
	    finite_d(st->lon, 0.0),
	    finite_f(st->acc, 999.0f),
	    finite_f(st->vacc, 0.0f),
	    finite_f(st->alt_msl, 0.0f),
	    finite_f(st->spd, 0.0f),
	    finite_f(st->hdg, 0.0f),
	    finite_f(st->pdop, 99.9f),
	    finite_f(st->hdop, 99.9f),
	    finite_f(st->vdop, 99.9f),
	    (unsigned long long)st->ts_ms,
	    st->used_total,
	    st->in_view_total,
	    st->used_by_sys[GNSS_SYS_GPS],     st->in_view_by_sys[GNSS_SYS_GPS],
	    st->used_by_sys[GNSS_SYS_GLONASS], st->in_view_by_sys[GNSS_SYS_GLONASS],
	    st->used_by_sys[GNSS_SYS_GALILEO], st->in_view_by_sys[GNSS_SYS_GALILEO],
	    st->used_by_sys[GNSS_SYS_BEIDOU],  st->in_view_by_sys[GNSS_SYS_BEIDOU],
	    st->used_by_sys[GNSS_SYS_QZSS],    st->in_view_by_sys[GNSS_SYS_QZSS],
	    st->used_by_sys[GNSS_SYS_NAVIC],   st->in_view_by_sys[GNSS_SYS_NAVIC],
	    st->used_by_sys[GNSS_SYS_SBAS],    st->in_view_by_sys[GNSS_SYS_SBAS]);

	for (uint32_t i = 0; i < st->num_svs; i++) {
		const struct tracked_sv *sv = &st->svs[i];
		buf_appendf(out, cap, &len,
		    "%s\n    {\"sys\":\"%s\",\"talker\":\"%s\",\"sv_id\":%u,\"prn\":%u,"
		    "\"elev\":%.1f,\"azim\":%.1f,\"snr\":%.1f,\"used\":%s,\"eph\":%s,\"alm\":%s}",
		    (i == 0) ? "" : ",",
		    gnss_sys_short_name((enum gnss_sys)sv->sys),
		    gnss_sys_talker((enum gnss_sys)sv->sys),
		    (unsigned)sv->canon_id,
		    (unsigned)sv->nmea_prn,
		    sv->has_elev ? finite_f(sv->elev, 0.0f) : 0.0f,
		    sv->has_azim ? finite_f(sv->azim, 0.0f) : 0.0f,
		    sv->has_snr  ? finite_f(sv->snr,  0.0f) : 0.0f,
		    sv->used ? "true" : "false",
		    sv->has_eph ? "true" : "false",
		    sv->has_alm ? "true" : "false");
	}
	buf_appendf(out, cap, &len, "%s]\n}\n", st->num_svs > 0 ? "\n  " : "");
	return len;
}

static void
write_telemetry_files(const struct gnss_state *st, char *json_buf, size_t json_cap,
    char *nmea_buf, size_t nmea_cap, size_t *out_nmea_len)
{
	size_t jlen = build_json_snapshot(st, json_buf, json_cap);
	if (jlen > 0) {
		(void)atomic_write_file(JSON_TMP_PATH, JSON_PATH, json_buf, jlen);
	}
	size_t nlen = build_nmea_snapshot(st, nmea_buf, nmea_cap);
	if (out_nmea_len) *out_nmea_len = nlen;
	if (nlen > 0 && atomic_write_file(NMEA_TMP_PATH, NMEA_PATH, nmea_buf, nlen) == 0) {
		notify_post(N_GPS_SV);
	}
}

static void
configure_multi_gnss_session(struct qmi_cli *cli, uint8_t sid)
{
	struct qmi_msg m;

	/* 1. Register as QMI LOC master client so constellation control/report configs are honored */
	uint32_t master_key = QMI_LOC_MASTER_CLIENT_KEY;
	m_init(&m, cli->txn++, QMI_LOC_REGISTER_MASTER_CLIENT_REQ);
	m_tlv(&m, 0x01, &master_key, sizeof(master_key));
	qmi_send(cli, &m);

	/* 2. Ensure GNSS engine is unlocked (1 = eQMI_LOC_LOCK_NONE_V02) */
	uint32_t lock_none = 1u;
	m_init(&m, cli->txn++, QMI_LOC_SET_ENGINE_LOCK_REQ);
	m_tlv(&m, 0x01, &lock_none, sizeof(lock_none));
	qmi_send(cli, &m);

	/* 3. Set default standalone+assisted operation mode (1 = eQMI_LOC_OPER_MODE_DEFAULT_V02) */
	uint32_t op_mode = 1u;
	m_init(&m, cli->txn++, QMI_LOC_SET_OPERATION_MODE_REQ);
	m_tlv(&m, 0x01, &op_mode, sizeof(op_mode));
	qmi_send(cli, &m);

	/*
	 * 4. Enable all secondary GNSS constellations alongside mandatory GPS:
	 *    GLONASS + BeiDou (BDS) + QZSS + Galileo + NavIC.
	 *    Note: disableMask (TLV 0x11) must NOT be sent (unsupported by Qualcomm modem).
	 */
	uint8_t reset_constell = 0;
	uint64_t ctrl_enable_mask = QMI_LOC_CTRL_CONSTELL_ALL;
	m_init(&m, cli->txn++, QMI_LOC_SET_CONSTELLATION_CONTROL_REQ);
	m_tlv(&m, 0x01, &reset_constell, sizeof(reset_constell));
	m_tlv(&m, 0x10, &ctrl_enable_mask, sizeof(ctrl_enable_mask));
	qmi_send(cli, &m);

	/* 5. Enable measurement & SV polynomial reporting for GPS+GLO+BDS+GAL+QZSS+NavIC */
	uint64_t report_constell_mask = QMI_LOC_REPORT_SYS_ALL;
	m_init(&m, cli->txn++, QMI_LOC_SET_GNSS_CONSTELL_REPORT_CONFIG);
	m_tlv(&m, 0x10, &report_constell_mask, sizeof(report_constell_mask));
	m_tlv(&m, 0x11, &report_constell_mask, sizeof(report_constell_mask));
	qmi_send(cli, &m);

	/* 6. Enable SBAS and QZSS-L1S augmentation */
	uint8_t sbas_on = 1u;
	m_init(&m, cli->txn++, QMI_LOC_SET_SBAS_CONFIG_REQ);
	m_tlv(&m, 0x01, &sbas_on, sizeof(sbas_on));
	m_tlv(&m, 0x10, &sbas_on, sizeof(sbas_on));
	qmi_send(cli, &m);

	/* 7. Enable multi-GNSS NMEA sentence mask ($GPGSV/$GLGSV/$GAGSV/$GBGSV/$GQGSV/$GIGSV/$GNGSA) */
	uint32_t nmea_mask = QMI_LOC_NMEA_MASK_MULTI_GNSS;
	m_init(&m, cli->txn++, QMI_LOC_SET_NMEA_TYPES_REQ);
	m_tlv(&m, 0x01, &nmea_mask, sizeof(nmea_mask));
	qmi_send(cli, &m);

	/* 8. Inject current UTC time to speed up cold TTFF (only if clock is past 2020) */
	struct timeval tv;
	gettimeofday(&tv, NULL);
	if (tv.tv_sec > 1577836800L) {
		uint64_t utc_ms = (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)(tv.tv_usec / 1000ULL);
		uint32_t unc_ms = 5000u;
		m_init(&m, cli->txn++, QMI_LOC_INJECT_UTC_REQ);
		m_tlv(&m, 0x01, &utc_ms, sizeof(utc_ms));
		m_tlv(&m, 0x02, &unc_ms, sizeof(unc_ms));
		qmi_send(cli, &m);
	}

	/* 9. Register for POSITION_REPORT (0x01) + GNSS_SV_INFO (0x02) 1 Hz indications */
	uint64_t ev_mask = QMI_LOC_EVENT_MASK_POS_REPORT | QMI_LOC_EVENT_MASK_GNSS_SV_INFO;
	m_init(&m, cli->txn++, QMI_LOC_REG_EVENTS_REQ);
	m_tlv(&m, 0x01, &ev_mask, sizeof(ev_mask));
	qmi_send(cli, &m);

	/*
	 * 10. Start periodic 1s high-accuracy GNSS session:
	 *     TLV 0x01: u8  sessionId
	 *     TLV 0x10: u32 fixRecurrence           = 1 (PERIODIC)
	 *     TLV 0x11: u32 horizontalAccuracyLevel = 3 (HIGH)
	 *     TLV 0x12: u32 intermediateReportState = 1 (ON)
	 *     TLV 0x13: u32 minInterval             = 1000 ms
	 */
	uint32_t recurrence = 1u;
	uint32_t accuracy_high = 3u;
	uint32_t intermed_on = 1u;
	uint32_t min_interval = 1000u;
	m_init(&m, cli->txn++, QMI_LOC_START_REQ);
	m_tlv(&m, 0x01, &sid, 1);
	m_tlv(&m, 0x10, &recurrence, 4);
	m_tlv(&m, 0x11, &accuracy_high, 4);
	m_tlv(&m, 0x12, &intermed_on, 4);
	m_tlv(&m, 0x13, &min_interval, 4);
	qmi_send(cli, &m);
}

int
main(int argc, char **argv)
{
	bool test_mode = false;
	bool print_nmea = false;
	int max_secs = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--test") == 0) {
			test_mode = true;
			if (i + 1 < argc && argv[i + 1][0] != '-') {
				max_secs = atoi(argv[++i]);
			}
		} else if (strcmp(argv[i], "--nmea") == 0) {
			print_nmea = true;
		}
	}

	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	struct gnss_state *state = calloc(1, sizeof(*state));
	uint8_t *rx_buf = malloc(QMI_RX_BUF_SIZE);
	char *json_buf = malloc(JSON_OUT_BUF_SIZE);
	char *nmea_buf = malloc(NMEA_OUT_BUF_SIZE);
	if (!state || !rx_buf || !json_buf || !nmea_buf) {
		fprintf(stderr, "sapphire-gpsd: out of memory\n");
		free(nmea_buf);
		free(json_buf);
		free(rx_buf);
		free(state);
		return 1;
	}
	state->session_status = 99;
	state->acc = 999.0f;
	state->pdop = 99.9f;
	state->hdop = 99.9f;
	state->vdop = 99.9f;

	struct qmi_cli cli;
	if (qmi_open_loc(&cli) != 0) {
		fprintf(stderr, "sapphire-gpsd: QMI LOC (0x10) not available\n");
		free(nmea_buf);
		free(json_buf);
		free(rx_buf);
		free(state);
		return 1;
	}
	fprintf(stderr, "sapphire-gpsd: connected to QMI LOC at %u:0x%x (GPS+GLO+GAL+BDS+QZSS+SBAS+NavIC)\n",
	    cli.node, cli.port);

	uint8_t sid = 1;
	configure_multi_gnss_session(&cli, sid);

	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (!g_stop) {
		if (max_secs > 0) {
			struct timespec now;
			clock_gettime(CLOCK_MONOTONIC, &now);
			if (now.tv_sec - t0.tv_sec >= max_secs) break;
		}
		fd_set rfds;
		FD_ZERO(&rfds);
		FD_SET(cli.fd, &rfds);
		struct timeval to = { .tv_sec = 1, .tv_usec = 0 };
		int sel = select(cli.fd + 1, &rfds, NULL, NULL, &to);
		if (sel <= 0) continue;

		ssize_t n = read(cli.fd, rx_buf, QMI_RX_BUF_SIZE);
		if (n <= (ssize_t)(sizeof(struct sweet_qmi_hdr) + 7u)) continue;
		const uint8_t *q = rx_buf + sizeof(struct sweet_qmi_hdr);
		uint32_t qlen = (uint32_t)(n - (ssize_t)sizeof(struct sweet_qmi_hdr));
		uint8_t flags = q[0];
		uint16_t msgid = 0;
		memcpy(&msgid, q + 3, 2);

		if (flags != 0x04) continue; /* Only process QMI indications */

		if (msgid == QMI_LOC_POS_REPORT_IND) {
			parse_pos_report_ind(state, q, qlen);
			if (state->have_fix && (state->session_status == 0 || state->session_status == 1)) {
				write_fix_file(state);
			}
			size_t nmea_len = 0;
			write_telemetry_files(state, json_buf, JSON_OUT_BUF_SIZE,
			    nmea_buf, NMEA_OUT_BUF_SIZE, &nmea_len);

			if (test_mode) {
				if (state->have_fix && (state->session_status == 0 || state->session_status == 1)) {
					printf("GNSS FIX (status=%u): lat=%.7f lon=%.7f acc=%.1fm alt=%.1fm spd=%.1fm/s hdg=%.1f "
					    "sats=%u/%u [GPS:%u/%u GLO:%u/%u GAL:%u/%u BDS:%u/%u QZSS:%u/%u SBAS:%u/%u NAVIC:%u/%u]\n",
					    state->session_status, state->lat, state->lon,
					    state->acc, state->alt_msl, state->spd, state->hdg,
					    state->used_total, state->in_view_total,
					    state->used_by_sys[GNSS_SYS_GPS],     state->in_view_by_sys[GNSS_SYS_GPS],
					    state->used_by_sys[GNSS_SYS_GLONASS], state->in_view_by_sys[GNSS_SYS_GLONASS],
					    state->used_by_sys[GNSS_SYS_GALILEO], state->in_view_by_sys[GNSS_SYS_GALILEO],
					    state->used_by_sys[GNSS_SYS_BEIDOU],  state->in_view_by_sys[GNSS_SYS_BEIDOU],
					    state->used_by_sys[GNSS_SYS_QZSS],    state->in_view_by_sys[GNSS_SYS_QZSS],
					    state->used_by_sys[GNSS_SYS_SBAS],    state->in_view_by_sys[GNSS_SYS_SBAS],
					    state->used_by_sys[GNSS_SYS_NAVIC],   state->in_view_by_sys[GNSS_SYS_NAVIC]);
				} else {
					printf("GNSS IND (status=%u, searching... in_view=%u [GPS:%u GLO:%u GAL:%u BDS:%u QZSS:%u SBAS:%u NAVIC:%u])\n",
					    state->session_status, state->in_view_total,
					    state->in_view_by_sys[GNSS_SYS_GPS],
					    state->in_view_by_sys[GNSS_SYS_GLONASS],
					    state->in_view_by_sys[GNSS_SYS_GALILEO],
					    state->in_view_by_sys[GNSS_SYS_BEIDOU],
					    state->in_view_by_sys[GNSS_SYS_QZSS],
					    state->in_view_by_sys[GNSS_SYS_SBAS],
					    state->in_view_by_sys[GNSS_SYS_NAVIC]);
				}
				if (print_nmea && nmea_len > 0) {
					fwrite(nmea_buf, 1, nmea_len, stdout);
				}
				fflush(stdout);
			}
		} else if (msgid == QMI_LOC_GNSS_SV_INFO_IND) {
			parse_sv_info_ind(state, q, qlen);
			if (state->have_fix) {
				write_fix_file(state);
			}
			size_t nmea_len = 0;
			write_telemetry_files(state, json_buf, JSON_OUT_BUF_SIZE,
			    nmea_buf, NMEA_OUT_BUF_SIZE, &nmea_len);

			if (test_mode) {
				printf("GNSS SVs: used=%u in_view=%u [GPS:%u/%u GLO:%u/%u GAL:%u/%u BDS:%u/%u QZSS:%u/%u SBAS:%u/%u NAVIC:%u/%u]\n",
				    state->used_total, state->in_view_total,
				    state->used_by_sys[GNSS_SYS_GPS],     state->in_view_by_sys[GNSS_SYS_GPS],
				    state->used_by_sys[GNSS_SYS_GLONASS], state->in_view_by_sys[GNSS_SYS_GLONASS],
				    state->used_by_sys[GNSS_SYS_GALILEO], state->in_view_by_sys[GNSS_SYS_GALILEO],
				    state->used_by_sys[GNSS_SYS_BEIDOU],  state->in_view_by_sys[GNSS_SYS_BEIDOU],
				    state->used_by_sys[GNSS_SYS_QZSS],    state->in_view_by_sys[GNSS_SYS_QZSS],
				    state->used_by_sys[GNSS_SYS_SBAS],    state->in_view_by_sys[GNSS_SYS_SBAS],
				    state->used_by_sys[GNSS_SYS_NAVIC],   state->in_view_by_sys[GNSS_SYS_NAVIC]);
				if (print_nmea && nmea_len > 0) {
					fwrite(nmea_buf, 1, nmea_len, stdout);
				}
				fflush(stdout);
			}
		} else if (msgid == QMI_LOC_NMEA_IND && print_nmea) {
			uint16_t l = 0;
			const uint8_t *p = tlv_find(q, qlen, 0x10, &l);
			if (!p || l == 0) {
				p = tlv_find(q, qlen, 0x01, &l);
			}
			if (p && l > 0) {
				fwrite(p, 1, l, stdout);
				fflush(stdout);
			}
		}
	}

	/* Stop GNSS session cleanly on exit */
	struct qmi_msg m;
	m_init(&m, cli.txn++, QMI_LOC_STOP_REQ);
	m_tlv(&m, 0x01, &sid, 1);
	qmi_send(&cli, &m);
	close(cli.fd);

	free(nmea_buf);
	free(json_buf);
	free(rx_buf);
	free(state);
	return 0;
}
