/*
 * libdivecomputer
 *
 * Copyright (C) 2012 Jef Driesen
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdlib.h>

#include <libdivecomputer/units.h>

#include "shearwater_predator.h"
#include "shearwater_petrel.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"
#include "field-cache.h"

#define ISINSTANCE(parser)	( \
	dc_parser_isinstance((parser), &shearwater_predator_parser_vtable) || \
	dc_parser_isinstance((parser), &shearwater_petrel_parser_vtable))

#define LOG_RECORD_DIVE_SAMPLE     0x01
#define LOG_RECORD_FREEDIVE_SAMPLE 0x02
#define LOG_RECORD_OPENING_0       0x10
#define LOG_RECORD_OPENING_1       0x11
#define LOG_RECORD_OPENING_2       0x12
#define LOG_RECORD_OPENING_3       0x13
#define LOG_RECORD_OPENING_4       0x14
#define LOG_RECORD_OPENING_5       0x15
#define LOG_RECORD_OPENING_6       0x16
#define LOG_RECORD_OPENING_7       0x17
#define LOG_RECORD_CLOSING_0       0x20
#define LOG_RECORD_CLOSING_1       0x21
#define LOG_RECORD_CLOSING_2       0x22
#define LOG_RECORD_CLOSING_3       0x23
#define LOG_RECORD_CLOSING_4       0x24
#define LOG_RECORD_CLOSING_5       0x25
#define LOG_RECORD_CLOSING_6       0x26
#define LOG_RECORD_CLOSING_7       0x27
#define LOG_RECORD_INFO_EVENT      0x30
#define LOG_RECORD_FINAL           0xFF

#define INFO_EVENT_TAG_LOG         38

#define SZ_BLOCK   0x80
#define SZ_SAMPLE_PREDATOR  0x10
#define SZ_SAMPLE_PETREL    0x20
#define SZ_SAMPLE_FREEDIVE  0x08

#define GASSWITCH     0x01
#define PPO2_EXTERNAL 0x02
#define SETPOINT_HIGH 0x04
#define SC            0x08
#define OC            0x10

#define METRIC   0
#define IMPERIAL 1

#define NGASMIXES 10
#define MAXSTRINGS 32
#define NTANKS    2
#define NRECORDS  8

#define PREDATOR 2
#define PETREL   3

#define UNDEFINED 0xFFFFFFFF

typedef struct shearwater_predator_parser_t shearwater_predator_parser_t;

typedef struct shearwater_predator_gasmix_t {
	unsigned int oxygen;
	unsigned int helium;
} shearwater_predator_gasmix_t;

typedef struct shearwater_predator_tank_t {
	unsigned int enabled;
	unsigned int beginpressure;
	unsigned int endpressure;
	unsigned int battery;
} shearwater_predator_tank_t;

struct shearwater_predator_parser_t {
	dc_parser_t base;
	unsigned int model;
	unsigned int petrel;
	unsigned int samplesize;
	// Cached fields.
	unsigned int cached;
	unsigned int pnf;
	unsigned int logversion;
	unsigned int headersize;
	unsigned int footersize;
	unsigned int opening[NRECORDS];
	unsigned int closing[NRECORDS];
	unsigned int final;
	unsigned int ngasmixes;
	unsigned int ntanks;
	shearwater_predator_gasmix_t gasmix[NGASMIXES];
	shearwater_predator_tank_t tank[NTANKS];
	unsigned int tankidx[NTANKS];
	unsigned int calibrated;
	double calibration[3];
	unsigned int serial;
	unsigned int units;
	unsigned int atmospheric;
	unsigned int density;

	/* Generic field cache */
	struct dc_field_cache cache;
};

static dc_status_t shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size);
static dc_status_t shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime);
static dc_status_t shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value);
static dc_status_t shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata);

static dc_status_t shearwater_predator_parser_cache (shearwater_predator_parser_t *parser);

static const dc_parser_vtable_t shearwater_predator_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PREDATOR,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};

static const dc_parser_vtable_t shearwater_petrel_parser_vtable = {
	sizeof(shearwater_predator_parser_t),
	DC_FAMILY_SHEARWATER_PETREL,
	shearwater_predator_parser_set_data, /* set_data */
	shearwater_predator_parser_get_datetime, /* datetime */
	shearwater_predator_parser_get_field, /* fields */
	shearwater_predator_parser_samples_foreach, /* samples_foreach */
	NULL /* destroy */
};


static unsigned int
shearwater_predator_find_gasmix (shearwater_predator_parser_t *parser, unsigned int o2, unsigned int he)
{
	unsigned int i = 0;
	while (i < parser->ngasmixes) {
		if (o2 == parser->gasmix[i].oxygen && he == parser->gasmix[i].helium)
			break;
		i++;
	}

	return i;
}


static dc_status_t
shearwater_common_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int serial, unsigned int petrel)
{
	shearwater_predator_parser_t *parser = NULL;
	const dc_parser_vtable_t *vtable = NULL;
	unsigned int samplesize = 0;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	if (petrel) {
		vtable = &shearwater_petrel_parser_vtable;
		samplesize = SZ_SAMPLE_PETREL;
	} else {
		vtable = &shearwater_predator_parser_vtable;
		samplesize = SZ_SAMPLE_PREDATOR;
	}

	// Allocate memory.
	parser = (shearwater_predator_parser_t *) dc_parser_allocate (context, vtable);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	// Set the default values.
	parser->model = model;
	parser->petrel = petrel;
	parser->samplesize = samplesize;
	parser->serial = serial;

	// Set the default values.
	parser->cached = 0;
	parser->pnf = 0;
	parser->logversion = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	for (unsigned int i = 0; i < NRECORDS; ++i) {
		parser->opening[i] = UNDEFINED;
		parser->closing[i] = UNDEFINED;
	}
	parser->final = UNDEFINED;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].enabled = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
		parser->tank[i].battery = 0;
		parser->tankidx[i] = i;
	}
	parser->calibrated = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		parser->calibration[i] = 0.0;
	}
	parser->units = METRIC;
	parser->density = 1025;
	parser->atmospheric = ATM / (BAR / 1000);

	DC_ASSIGN_FIELD(parser->cache, DIVEMODE, DC_DIVEMODE_OC);

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}


dc_status_t
shearwater_predator_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int serial)
{
	return shearwater_common_parser_create (out, context, model, serial, 0);
}


dc_status_t
shearwater_petrel_parser_create (dc_parser_t **out, dc_context_t *context, unsigned int model, unsigned int serial)
{
	return shearwater_common_parser_create (out, context, model, serial, 1);
}


static dc_status_t
shearwater_predator_parser_set_data (dc_parser_t *abstract, const unsigned char *data, unsigned int size)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	// Reset the cache.
	parser->cached = 0;
	parser->pnf = 0;
	parser->logversion = 0;
	parser->headersize = 0;
	parser->footersize = 0;
	for (unsigned int i = 0; i < NRECORDS; ++i) {
		parser->opening[i] = UNDEFINED;
		parser->closing[i] = UNDEFINED;
	}
	parser->final = UNDEFINED;
	parser->ngasmixes = 0;
	for (unsigned int i = 0; i < NGASMIXES; ++i) {
		parser->gasmix[i].oxygen = 0;
		parser->gasmix[i].helium = 0;
	}
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NTANKS; ++i) {
		parser->tank[i].enabled = 0;
		parser->tank[i].beginpressure = 0;
		parser->tank[i].endpressure = 0;
		parser->tankidx[i] = i;
	}
	parser->calibrated = 0;
	for (unsigned int i = 0; i < 3; ++i) {
		parser->calibration[i] = 0.0;
	}
	parser->units = METRIC;
	parser->density = 1025;
	parser->atmospheric = ATM / (BAR / 1000);

	DC_ASSIGN_FIELD(parser->cache, DIVEMODE, DC_DIVEMODE_OC);

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_get_datetime (dc_parser_t *abstract, dc_datetime_t *datetime)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;
	const unsigned char *data = abstract->data;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	unsigned int ticks = array_uint32_be (data + parser->opening[0] + 12);

	if (!dc_datetime_gmtime (datetime, ticks))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
}

// Show the battery state
//
// NOTE! Right now it only shows the most serious bit
// but the code is set up so that we could perhaps
// indicate that the battery is on the edge (ie it
// reported both "normal" _and_ "warning" during the
// dive - maybe that would be a "starting to warn")
//
// We could also report unpaired and comm errors.
static void
add_battery_info(shearwater_predator_parser_t *parser, const char *desc, unsigned int state)
{
	// We don't know what other state bits than 0-2 mean
	state &= 7;
	if (state >= 1 && state <= 7) {
		static const char *states[8] = {
			"",		// 000 - No state bits, not used
			"normal",	// 001 - only normal
			"critical",	// 010 - only critical
			"critical",	// 011 - both normal and critical
			"warning",	// 100 - only warning
			"warning",	// 101 - normal and warning
			"critical",	// 110 - warning and critical
			"critical",	// 111 - normal, warning and critical
		};
		dc_field_add_string(&parser->cache, desc, states[state]);
	}
}

static void
add_deco_model(shearwater_predator_parser_t *parser, const unsigned char *data)
{
	unsigned int idx_deco_model = parser->pnf ? parser->opening[2] + 18 : 67;
	unsigned int idx_gfs = parser->pnf ? parser->opening[3] + 5 : 85;

	switch	(data[idx_deco_model]) {
	case 0:
		dc_field_add_string_fmt(&parser->cache, "Deco model", "GF %u/%u", data[4], data[5]);
		break;
	case 1:
		dc_field_add_string_fmt(&parser->cache, "Deco model", "VPM-B +%u", data[idx_deco_model + 1]);
		break;
	case 2:
		dc_field_add_string_fmt(&parser->cache, "Deco model", "VPM-B/GFS +%u %u%%", data[idx_deco_model + 1], data[idx_gfs]);
		break;
	default:
		dc_field_add_string_fmt(&parser->cache, "Deco model", "Unknown model %d", data[idx_deco_model]);
	}
}

static void
add_battery_type(shearwater_predator_parser_t *parser, const unsigned char *data)
{
	if (parser->logversion < 7)
		return;

	unsigned int idx_battery_type = parser->pnf ? parser->opening[4] + 9 : 120;
	switch (data[idx_battery_type]) {
	case 1:
		dc_field_add_string(&parser->cache, "Battery type", "1.5V Alkaline");
		break;
	case 2:
		dc_field_add_string(&parser->cache, "Battery type", "1.5V Lithium");
		break;
	case 3:
		dc_field_add_string(&parser->cache, "Battery type", "1.2V NiMH");
		break;
	case 4:
		dc_field_add_string(&parser->cache, "Battery type", "3.6V Saft");
		break;
	case 5:
		dc_field_add_string(&parser->cache, "Battery type", "3.7V Li-Ion");
		break;
	default:
		dc_field_add_string_fmt(&parser->cache, "Battery type", "unknown type %d", data[idx_battery_type]);
		break;
	}
}

static dc_status_t
shearwater_predator_parser_cache (shearwater_predator_parser_t *parser)
{
	dc_parser_t *abstract = (dc_parser_t *) parser;
	const unsigned char *data = parser->base.data;
	unsigned int size = parser->base.size;

	if (parser->cached) {
		return DC_STATUS_SUCCESS;
	}
	memset(&parser->cache, 0, sizeof(parser->cache));

	// Log versions before 6 weren't reliably stored in the data, but
	// 6 is also the oldest version that we assume in our code
	unsigned int logversion = 0;

	// Verify the minimum length.
	if (size < 2) {
		ERROR (abstract->context, "Invalid data length.");
		return DC_STATUS_DATAFORMAT;
	}

	// The Petrel Native Format (PNF) is very similar to the legacy
	// Predator and Predator-like format. The samples are simply offset
	// by one (so we can use pnf as the offset). For the header and
	// footer data, it's more complicated because of the new 32 byte
	// block structure.
	unsigned int pnf = parser->petrel ? array_uint16_be (data) != 0xFFFF : 0;
	unsigned int headersize = 0;
	unsigned int footersize = 0;
	if (!pnf) {
		// Opening and closing blocks.
		headersize = SZ_BLOCK;
		footersize = SZ_BLOCK;
		if (size < headersize + footersize) {
			ERROR (abstract->context, "Invalid data length.");
			return DC_STATUS_DATAFORMAT;
		}

		// Adjust the footersize for the final block.
		if (parser->petrel || array_uint16_be (data + size - footersize) == 0xFFFD) {
			footersize += SZ_BLOCK;
			if (size < headersize + footersize) {
				ERROR (abstract->context, "Invalid data length.");
				return DC_STATUS_DATAFORMAT;
			}

			parser->final = size - SZ_BLOCK;
		}

		// The Predator and Predator-like format have just one large 128
		// byte opening and closing block. To minimize the differences
		// with the PNF format, all record offsets are assigned the same
		// value here.
		for (unsigned int i = 0; i < NRECORDS; ++i) {
			parser->opening[i] = 0;
			parser->closing[i] = size - footersize;
		}

		// Log version
		logversion = data[127];
	}

	// Default dive mode.
	dc_divemode_t mode = DC_DIVEMODE_OC;

	// Get the gas mixes.
	unsigned int ngasmixes = 0;
	shearwater_predator_gasmix_t gasmix[NGASMIXES] = {0};
	shearwater_predator_tank_t tank[NTANKS] = {0};
	unsigned int o2_previous = 0, he_previous = 0;

	unsigned int offset = headersize;
	unsigned int length = size - footersize;
	while (offset + parser->samplesize <= length) {
		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Get the record type.
		unsigned int type = pnf ? data[offset] : LOG_RECORD_DIVE_SAMPLE;

		if (type == LOG_RECORD_DIVE_SAMPLE) {
			// Status flags.
			unsigned int status = data[offset + 11 + pnf];
			if ((status & OC) == 0) {
				mode = DC_DIVEMODE_CCR;
			}

			// Gaschange.
			unsigned int o2 = data[offset + 7 + pnf];
			unsigned int he = data[offset + 8 + pnf];
			if (o2 != o2_previous || he != he_previous) {
				// Find the gasmix in the list.
				unsigned int idx = 0;
				while (idx < ngasmixes) {
					if (o2 == gasmix[idx].oxygen && he == gasmix[idx].helium)
						break;
					idx++;
				}

				// Add it to list if not found.
				if (idx >= ngasmixes) {
					if (idx >= NGASMIXES) {
						ERROR (abstract->context, "Maximum number of gas mixes reached.");
						return DC_STATUS_NOMEMORY;
					}
					gasmix[idx].oxygen = o2;
					gasmix[idx].helium = he;
					ngasmixes = idx + 1;
				}

				o2_previous = o2;
				he_previous = he;
			}

			// Tank pressure
			if (logversion >= 7) {
				const unsigned int idx[NTANKS] = {27, 19};
				for (unsigned int i = 0; i < NTANKS; ++i) {
					// Values above 0xFFF0 are special codes:
					//    0xFFFF AI is off
					//    0xFFFE No comms for 90 seconds+
					//    0xFFFD No comms for 30 seconds
					//    0xFFFC Transmitter not paired
					// For regular values, the top 4 bits contain the battery
					// level (0=normal, 1=critical, 2=warning), and the lower 12
					// bits the tank pressure in units of 2 psi.
					unsigned int pressure = array_uint16_be (data + offset + pnf + idx[i]);
					if (pressure < 0xFFF0) {
						unsigned int battery = 1u << (pressure >> 12);
						pressure &= 0x0FFF;
						if (!tank[i].enabled) {
							tank[i].enabled = 1;
							tank[i].beginpressure = pressure;
							tank[i].endpressure = pressure;
							tank[i].battery = 0;
						}
						tank[i].endpressure = pressure;
						tank[i].battery |= battery;
					}
				}
			}
		} else if (type == LOG_RECORD_FREEDIVE_SAMPLE) {
			// Freedive record
			mode = DC_DIVEMODE_FREEDIVE;
		} else if (type >= LOG_RECORD_OPENING_0 && type <= LOG_RECORD_OPENING_7) {
			// Opening record
			parser->opening[type - LOG_RECORD_OPENING_0] = offset;

			// Log version
			if (type == LOG_RECORD_OPENING_4) {
				logversion = data[offset + 16];
			}
		} else if (type >= LOG_RECORD_CLOSING_0 && type <= LOG_RECORD_CLOSING_7) {
			// Closing record
			parser->closing[type - LOG_RECORD_CLOSING_0] = offset;
		} else if (type == LOG_RECORD_FINAL) {
			// Final record
			parser->final = offset;
		}

		offset += parser->samplesize;
	}

	// Verify the required opening/closing records.
	// At least in firmware v71 and newer, Petrel and Petrel 2 also use PNF,
	// and there opening/closing record 5 (which contains AI information plus
	// the sample interval) don't appear to exist - so don't mark them as required
	for (unsigned int i = 0; i <= 4; ++i) {
		if (parser->opening[i] == UNDEFINED || parser->closing[i] == UNDEFINED) {
			ERROR (abstract->context, "Opening or closing record %u not found.", i);
			return DC_STATUS_DATAFORMAT;
		}
	}

	dc_field_add_string_fmt(&parser->cache, "Logversion", "%d%s", logversion, pnf ? "(PNF)" : "");

	// Cache sensor calibration for later use
	unsigned int nsensors = 0, ndefaults = 0;
	unsigned int base = parser->opening[3] + (pnf ? 6 : 86);
	for (size_t i = 0; i < 3; ++i) {
		unsigned int calibration = array_uint16_be(data + base + 1 + i * 2);
		parser->calibration[i] = calibration / 100000.0;
		if (parser->model == PREDATOR) {
			// The Predator expects the mV output of the cells to be
			// within 30mV to 70mV in 100% O2 at 1 atmosphere. If the
			// calibration value is scaled with a factor 2.2, then the
			// sensors lines up and matches the average.
			parser->calibration[i] *= 2.2;
		}
		if (data[base] & (1 << i)) {
			if (calibration == 2100) {
				ndefaults++;
			}
			nsensors++;
		}
	}
	if (nsensors && nsensors == ndefaults) {
		// If all (calibrated) sensors still have their factory default
		// calibration values (2100), they are probably not calibrated
		// properly. To avoid returning incorrect ppO2 values to the
		// application, they are manually disabled (e.g. marked as
		// uncalibrated).
		WARNING (abstract->context, "Disabled all O2 sensors due to a default calibration value.");
		parser->calibrated = 0;
		if (mode != DC_DIVEMODE_OC)
			dc_field_add_string(&parser->cache, "PPO2 source", "voted/averaged");
	} else {
		parser->calibrated = data[base];
		if (mode != DC_DIVEMODE_OC)
			dc_field_add_string(&parser->cache, "PPO2 source", "cells");
	}

	// Cache the data for later use.
	parser->pnf = pnf;
	parser->logversion = logversion;
	parser->headersize = headersize;
	parser->footersize = footersize;
	parser->ngasmixes = ngasmixes;
	for (unsigned int i = 0; i < ngasmixes; ++i) {
		parser->gasmix[i] = gasmix[i];
	}
	parser->ntanks = 0;
	for (unsigned int i = 0; i < NTANKS; ++i) {
		if (tank[i].enabled) {
			parser->tankidx[i] = parser->ntanks;
			parser->tank[parser->ntanks] = tank[i];
			parser->ntanks++;
		} else {
			parser->tankidx[i] = UNDEFINED;
		}
	}
	parser->units = data[parser->opening[0] + 8];
	parser->atmospheric = array_uint16_be (data + parser->opening[1] + (parser->pnf ? 16 : 47));
	parser->density = array_uint16_be (data + parser->opening[3] + (parser->pnf ? 3 : 83));
	parser->cached = 1;

	DC_ASSIGN_FIELD(parser->cache, DIVEMODE, mode);

	dc_field_add_string_fmt(&parser->cache, "Serial", "%08x", parser->serial);
	// bytes 1-31 are identical in all formats
	dc_field_add_string_fmt(&parser->cache, "FW Version", "%2x", data[19]);
	add_deco_model(parser, data);
	add_battery_type(parser, data);
	dc_field_add_string_fmt(&parser->cache, "Battery at end", "%.1f V", data[9] / 10.0);
	add_battery_info(parser, "T1 battery", tank[0].battery);
	add_battery_info(parser, "T2 battery", tank[1].battery);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
shearwater_predator_parser_get_field (dc_parser_t *abstract, dc_field_type_t type, unsigned int flags, void *value)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	dc_gasmix_t *gasmix = (dc_gasmix_t *) value;
	dc_tank_t *tank = (dc_tank_t *) value;
	dc_salinity_t *water = (dc_salinity_t *) value;
	dc_field_string_t *string = (dc_field_string_t *) value;

	if (value) {
		switch (type) {
		case DC_FIELD_DIVETIME:
			if (parser->pnf)
				*((unsigned int *) value) = array_uint24_be (data + parser->closing[0] + 6);
			else
				*((unsigned int *) value) = array_uint16_be (data + parser->closing[0] + 6) * 60;
			break;
		case DC_FIELD_MAXDEPTH:
			if (parser->units == IMPERIAL)
				*((double *) value) = array_uint16_be (data + parser->closing[0] + 4) * FEET;
			else
				*((double *) value) = array_uint16_be (data + parser->closing[0] + 4);
			if (parser->pnf)
				*((double *)value) /= 10.0;
			break;
		case DC_FIELD_GASMIX_COUNT:
			*((unsigned int *) value) = parser->ngasmixes;
			break;
		case DC_FIELD_GASMIX:
			gasmix->oxygen = parser->gasmix[flags].oxygen / 100.0;
			gasmix->helium = parser->gasmix[flags].helium / 100.0;
			gasmix->nitrogen = 1.0 - gasmix->oxygen - gasmix->helium;
			break;
		case DC_FIELD_TANK_COUNT:
			*((unsigned int *) value) = parser->ntanks;
			break;
		case DC_FIELD_TANK:
			tank->type = DC_TANKVOLUME_NONE;
			tank->volume = 0.0;
			tank->workpressure = 0.0;
			tank->beginpressure = parser->tank[flags].beginpressure * 2 * PSI / BAR;
			tank->endpressure   = parser->tank[flags].endpressure   * 2 * PSI / BAR;
			tank->gasmix = DC_GASMIX_UNKNOWN;
			break;
		case DC_FIELD_SALINITY:
			if (parser->density == 1000)
				water->type = DC_WATER_FRESH;
			else
				water->type = DC_WATER_SALT;
			water->density = parser->density;
			break;
		case DC_FIELD_ATMOSPHERIC:
			*((double *) value) = parser->atmospheric / 1000.0;
			break;
		case DC_FIELD_DIVEMODE:
			return DC_FIELD_VALUE(parser->cache, value, DIVEMODE);
		case DC_FIELD_STRING:
			return dc_field_get_string(&parser->cache, flags, string);
		default:
			return DC_STATUS_UNSUPPORTED;
		}
	}

	return DC_STATUS_SUCCESS;
}


static dc_status_t
shearwater_predator_parser_samples_foreach (dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	shearwater_predator_parser_t *parser = (shearwater_predator_parser_t *) abstract;

	const unsigned char *data = abstract->data;
	unsigned int size = abstract->size;

	// Cache the parser data.
	dc_status_t rc = shearwater_predator_parser_cache (parser);
	if (rc != DC_STATUS_SUCCESS)
		return rc;

	// Previous gas mix.
	unsigned int o2_previous = 0, he_previous = 0;

	// Sample interval.
	unsigned int time = 0;
	unsigned int interval = 10;
	if (parser->pnf && parser->logversion >= 9 && parser->opening[5] != UNDEFINED) {
		interval = array_uint16_be (data + parser->opening[5] + 23);
		if (interval % 1000 != 0) {
			ERROR (abstract->context, "Unsupported sample interval (%u ms).", interval);
			return DC_STATUS_DATAFORMAT;
		}
		interval /= 1000;
	}

	unsigned int pnf = parser->pnf;
	unsigned int offset = parser->headersize;
	unsigned int length = size - parser->footersize;
	while (offset + parser->samplesize <= length) {
		dc_sample_value_t sample = {0};

		// Ignore empty samples.
		if (array_isequal (data + offset, parser->samplesize, 0x00)) {
			offset += parser->samplesize;
			continue;
		}

		// Get the record type.
		unsigned int type = pnf ? data[offset] : LOG_RECORD_DIVE_SAMPLE;

		// stop parsing if we see the end block
		if (type == LOG_RECORD_FINAL && data[offset + 1] == 0xFD)
			break;

		if (type == LOG_RECORD_DIVE_SAMPLE) {
			// Time (seconds).
			time += interval;
			sample.time = time;
			if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

			// Depth (1/10 m or ft).
			unsigned int depth = array_uint16_be (data + pnf + offset);
			if (parser->units == IMPERIAL)
				sample.depth = depth * FEET / 10.0;
			else
				sample.depth = depth / 10.0;
			if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

			// Temperature (°C or °F).
			int temperature = (signed char) data[offset + pnf + 13];
			if (temperature < 0) {
				// Fix negative temperatures.
				temperature += 102;
				if (temperature > 0) {
					temperature = 0;
				}
			}
			if (parser->units == IMPERIAL)
				sample.temperature = (temperature - 32.0) * (5.0 / 9.0);
			else
				sample.temperature = temperature;
			if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);

			// Status flags.
			unsigned int status = data[offset + pnf + 11];

			if ((status & OC) == 0) {
				// PPO2
				if ((status & PPO2_EXTERNAL) == 0) {
					if (!parser->calibrated) {
						sample.ppo2 = data[offset + pnf + 6] / 100.0;
						if (callback) callback (DC_SAMPLE_PPO2, sample, userdata);
					} else {
						sample.ppo2 = data[offset + pnf + 12] * parser->calibration[0];
						if (callback && (parser->calibrated & 0x01)) callback (DC_SAMPLE_PPO2, sample, userdata);

						sample.ppo2 = data[offset + pnf + 14] * parser->calibration[1];
						if (callback && (parser->calibrated & 0x02)) callback (DC_SAMPLE_PPO2, sample, userdata);

						sample.ppo2 = data[offset + pnf + 15] * parser->calibration[2];
						if (callback && (parser->calibrated & 0x04)) callback (DC_SAMPLE_PPO2, sample, userdata);
					}
				}

				// Setpoint
				if (parser->petrel) {
					sample.setpoint = data[offset + pnf + 18] / 100.0;
				} else {
					// this will only ever be called for the actual Predator, so no adjustment needed for PNF
					if (status & SETPOINT_HIGH) {
						sample.setpoint = data[18] / 100.0;
					} else {
						sample.setpoint = data[17] / 100.0;
					}
				}
				if (callback) callback (DC_SAMPLE_SETPOINT, sample, userdata);
			}

			// CNS
			if (parser->petrel) {
				sample.cns = data[offset + pnf + 22] / 100.0;
				if (callback) callback (DC_SAMPLE_CNS, sample, userdata);
			}

			// Gaschange.
			unsigned int o2 = data[offset + pnf + 7];
			unsigned int he = data[offset + pnf + 8];
			if (o2 != o2_previous || he != he_previous) {
				unsigned int idx = shearwater_predator_find_gasmix (parser, o2, he);
				if (idx >= parser->ngasmixes) {
					ERROR (abstract->context, "Invalid gas mix.");
					return DC_STATUS_DATAFORMAT;
				}

				sample.gasmix = idx;
				if (callback) callback (DC_SAMPLE_GASMIX, sample, userdata);
				o2_previous = o2;
				he_previous = he;
			}

			// Deco stop / NDL.
			unsigned int decostop = array_uint16_be (data + offset + pnf + 2);
			if (decostop) {
				sample.deco.type = DC_DECO_DECOSTOP;
				if (parser->units == IMPERIAL)
					sample.deco.depth = decostop * FEET;
				else
					sample.deco.depth = decostop;
			} else {
				sample.deco.type = DC_DECO_NDL;
				sample.deco.depth = 0.0;
			}
			sample.deco.time = data[offset + pnf + 9] * 60;
			if (callback) callback (DC_SAMPLE_DECO, sample, userdata);

			// for logversion 7 and newer (introduced for Perdix AI)
			// detect tank pressure
			if (parser->logversion >= 7) {
				const unsigned int idx[NTANKS] = {27, 19};
				for (unsigned int i = 0; i < NTANKS; ++i) {
					// Tank pressure
					// Values above 0xFFF0 are special codes:
					//    0xFFFF AI is off
					//    0xFFFE No comms for 90 seconds+
					//    0xFFFD No comms for 30 seconds
					//    0xFFFC Transmitter not paired
					// For regular values, the top 4 bits contain the battery
					// level (0=normal, 1=critical, 2=warning), and the lower 12
					// bits the tank pressure in units of 2 psi.
					unsigned int pressure = array_uint16_be (data + offset + pnf + idx[i]);
					if (pressure < 0xFFF0) {
						pressure &= 0x0FFF;
						sample.pressure.tank = parser->tankidx[i];
						sample.pressure.value = pressure * 2 * PSI / BAR;
						if (callback) callback (DC_SAMPLE_PRESSURE, sample, userdata);
					}
				}

				// Gas time remaining in minutes
				// Values above 0xF0 are special codes:
				//    0xFF Not paired
				//    0xFE No communication
				//    0xFD Not available in current mode
				//    0xFC Not available because of DECO
				//    0xFB Tank size or max pressure haven’t been set up
				if (data[offset + pnf + 21] < 0xF0) {
					sample.rbt = data[offset + pnf + 21];
					if (callback) callback (DC_SAMPLE_RBT, sample, userdata);
				}
			}
		} else if (type == LOG_RECORD_FREEDIVE_SAMPLE) {
			// A freedive record is actually 4 samples, each 8-bytes,
			// packed into a standard 32-byte sized record. At the end
			// of a dive, unused partial records will be 0 padded.
			for (unsigned int i = 0; i < 4; ++i) {
				unsigned int idx = offset + i * SZ_SAMPLE_FREEDIVE;

				// Ignore empty samples.
				if (array_isequal (data + idx, SZ_SAMPLE_FREEDIVE, 0x00)) {
					break;
				}

				// Time (seconds).
				time += interval;
				sample.time = time;
				if (callback) callback (DC_SAMPLE_TIME, sample, userdata);

				// Depth (absolute pressure in millibar)
				unsigned int depth = array_uint16_be (data + idx + 1);
				sample.depth = (signed int)(depth - parser->atmospheric) * (BAR / 1000.0) / (parser->density * GRAVITY);
				if (callback) callback (DC_SAMPLE_DEPTH, sample, userdata);

				// Temperature (1/10 °C).
				int temperature = (signed short) array_uint16_be (data + idx + 3);
				sample.temperature = temperature / 10.0;
				if (callback) callback (DC_SAMPLE_TEMPERATURE, sample, userdata);
			}
		} else if (type == LOG_RECORD_INFO_EVENT) {
			unsigned int event = data[offset + 1];
			unsigned int timestamp = array_uint32_be (data + offset + 4);
			unsigned int w1 = array_uint32_be (data + offset + 8);
			unsigned int w2 = array_uint32_be (data + offset + 12);

			if (event == INFO_EVENT_TAG_LOG) {
				// Compass heading
				if (w1 != 0xFFFFFFFF) {
					sample.bearing = w1;
					if (callback) callback (DC_SAMPLE_BEARING, sample, userdata);
				}

				// Tag
				sample.event.type = SAMPLE_EVENT_BOOKMARK;
				sample.event.time = 0;
				sample.event.flags = 0;
				sample.event.value = w2;
				if (callback) callback (DC_SAMPLE_EVENT, sample, userdata);
			}
		}

		offset += parser->samplesize;
	}

	return DC_STATUS_SUCCESS;
}
