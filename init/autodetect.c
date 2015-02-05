#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>

#include <unistd.h>
#include <string.h>
#include <ctype.h>


#include <stdint.h>
#include <linux/types.h>
#include <linux/fb.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include "log.h"
#include "property_service.h"
#include <sys/system_properties.h>
#include "efivars.h"


#define u8 __u8
#define u32 __u32

#include "drm_edid.h"

static int pixels_x, pixels_y;
/*
 * if we don't know the size of the screen, we assume a 8" 16:9 screen
 * bigger screens than 8" tend to be eDP/LVDS which we will get actual sizes
 * from EDID; it's the small cheap ones where we won't know.
 */
static int mm_x = 177, mm_y = 99;
static int mm_set = 0;
static int current_rank = 100;
static char *origin = NULL;

static int dpi = 0, rawdpi = 0;

static int verbose;


static int get_rank(char *file)
{
	if (strstr(file, "eDP"))
		return 10;
	if (strstr(file, "LVDS"))
		return 10;
	if (strstr(file, "DSI"))
		return 10;

	if (strstr(file, "HDMI"))
		return 200;

	return 50;
}

static void push_resolution(int x, int y, int m_x, int m_y, int rank, char *__origin)
{

	if (x < pixels_x || y < pixels_y)
		return;

	if (rank > current_rank && mm_set > 0)
		return;

	if (m_x > 0 && m_y > 0) {
		mm_x = m_x;
		mm_y = m_y;
		mm_set = 1;
	}

	if (rank > current_rank)
		return;

	pixels_x = x;
	pixels_y = y;

	if (m_x > 0 && m_y > 0) {
		mm_x = m_x;
		mm_y = m_y;
		mm_set = 1;
	}

	origin = __origin;
}

static int valid_edid_header(struct edid *edid)
{
	int i;
	unsigned char sum = 0, *ptr;
	if (edid->header[0] != 0)	return 0;
	if (edid->header[1] != 0xff)	return 0;
	if (edid->header[2] != 0xff)	return 0;
	if (edid->header[3] != 0xff)	return 0;
	if (edid->header[4] != 0xff)	return 0;
	if (edid->header[5] != 0xff)	return 0;
	if (edid->header[6] != 0xff)	return 0;
	if (edid->header[7] != 0)	return 0;

	ptr = (unsigned char *)edid;
	for (i = 0; i < 128; i++)
		sum += ptr[i];

	if (sum != 0)
		ERROR("edid: Invalid checksum\n");

	return 1;
}

static int vsize(int x, int vfreq_aspect)
{
	int y;
	y = x;
	vfreq_aspect = vfreq_aspect >> 6;
	if (vfreq_aspect == 0)
		y = x * 10 / 16;
	if (vfreq_aspect == 1)
		y = x * 3 / 4;
	if (vfreq_aspect == 2)
		y = x * 4 / 5;
	if (vfreq_aspect == 3)
		y = x * 9 / 16;

	return y;
}

/*
 * Device implementations SHOULD define the standard Android framework
 * density that is numerically closest to the physical density of the
 * screen */
static int snap_dpi(double dpi) {
	if (dpi < 140)
		return 120;
	if (dpi < 187)
		return 160;
	if (dpi < 227)
		return 213;
	if (dpi < 280)
		return 240;
	if (dpi < 360)
		return 320;
	if (dpi < 440)
		return 400;
	if (dpi < 560)
		return 480;

	return 640;
}

static void dpi_math(void)
{
	double xdpi, ydpi, adpi;

	/* detect misrotated dimensions */
	if ((mm_x > mm_y) && (pixels_x < pixels_y)) {
		int t;
		t = mm_x;
		mm_x = mm_y;
		mm_y = t;
	}

	ERROR("edid: Final screen info:   %ix%i pixels, %ix%i mm\n", pixels_x, pixels_y, mm_x, mm_y);

	if (mm_x == 0)
		return;
	if (mm_y == 0)
		return;

	xdpi = 1.0 * pixels_x / (mm_x / 25.4);
	ydpi = 1.0 * pixels_y / (mm_y / 25.4);

	adpi = xdpi;
	if (ydpi > adpi)
		adpi = ydpi;

	ERROR("edid: dpi   %5.2f, %5.2f for an converged dpi of %5.2f  \n", xdpi, ydpi, adpi);
	rawdpi = adpi;
	dpi = snap_dpi(adpi);
	ERROR("edid: Final DPI is %i  \n", dpi);
}

static void parse_edid(char *filename)
{
	FILE *file;
	struct edid edid;
	int ret;
	int i;

	file = fopen(filename, "r");
	if (!file) {
		ERROR("edid: Cannot open %s\n", filename);
		return;
	}
	ret = fread(&edid, 128, 1, file);
	if (ret != 1) {
		fclose(file);
		ERROR("edid: Edid read failed: %i (%s)\n", ret, filename);
		return;
	}
	fclose(file);

	if (!valid_edid_header(&edid)) {
		ERROR("edid: Invalid EDID header  : %02x%02x%02x%02x%02x%02x%02x%02x\n", edid.header[0], edid.header[1], edid.header[2], edid.header[3], edid.header[4], edid.header[5], edid.header[6], edid.header[7]);
		return;
	}

	ERROR("edid: Edid version : %i.%i\n", edid.version, edid.revision);

	for (i = 0; i < 4; i++)
		if (edid.detailed_timings[i].pixel_clock) {
			push_resolution(
				edid.detailed_timings[i].data.pixel_data.hactive_lo + ((edid.detailed_timings[i].data.pixel_data.hactive_hblank_hi >> 4) << 8),
				edid.detailed_timings[i].data.pixel_data.vactive_lo + ((edid.detailed_timings[i].data.pixel_data.vactive_vblank_hi >> 4) << 8),
				edid.detailed_timings[i].data.pixel_data.width_mm_lo + ((edid.detailed_timings[i].data.pixel_data.width_height_mm_hi >> 4) << 8),
				edid.detailed_timings[i].data.pixel_data.height_mm_lo + ((edid.detailed_timings[i].data.pixel_data.width_height_mm_hi & 15) << 8),
				get_rank(filename),
				"EDID detailed timings"
				       );

			ERROR("edid:     %i x %i pixels  \n",  edid.detailed_timings[i].data.pixel_data.hactive_lo + ((edid.detailed_timings[i].data.pixel_data.hactive_hblank_hi >> 4) << 8),
				   			   edid.detailed_timings[i].data.pixel_data.vactive_lo + ((edid.detailed_timings[i].data.pixel_data.vactive_vblank_hi >> 4) << 8));

			ERROR("edid:     %imm x %imm\n", 	   edid.detailed_timings[i].data.pixel_data.width_mm_lo + ((edid.detailed_timings[i].data.pixel_data.width_height_mm_hi >> 4) << 8),
					 edid.detailed_timings[i].data.pixel_data.height_mm_lo + ((edid.detailed_timings[i].data.pixel_data.width_height_mm_hi & 15) << 8));
		}

	for (i = 0; i < 8; i++)
		if (edid.standard_timings[i].hsize && (edid.standard_timings[i].hsize != 1 || edid.standard_timings[i].vfreq_aspect != 1) ) {
			ERROR("edid:     %i x %i\n", (edid.standard_timings[i].hsize + 31) * 8, vsize((edid.standard_timings[i].hsize + 31) * 8, edid.standard_timings[i].vfreq_aspect) );
			ERROR("edid:     %imm x %imm \n", edid.width_cm * 10, edid.height_cm * 10);

			push_resolution( (edid.standard_timings[i].hsize + 31) * 8, vsize((edid.standard_timings[i].hsize + 31) * 8, edid.standard_timings[i].vfreq_aspect),
						edid.width_cm * 10, edid.height_cm * 10,
						get_rank(filename) + 5, "EDID legacy timings");
		}
}

static void parse_display_info(char *filename)
{
	FILE *file;
	file = fopen(filename, "r");
	if (!file) {
		ERROR("edid: cannot open %s\n", filename);
		return;
	}
	while (!feof(file)) {
		char *c;
		char line[PATH_MAX];
		memset(line, 0, PATH_MAX);
		if (fgets(line, PATH_MAX, file) == NULL)
			break;
		c = strchr(line, ':');
		if (c && strstr(line, "physical dimensions")) {
			int x, y;
			while (*c == ' ' || *c == ':') c++;

			x = strtoull(c, NULL, 10);
			if (!x)
				break;
			c = strchr(c, 'x');
			if (!c)
				break;
			c++;
			y = strtoull(c, NULL, 10);
			if (x && y && current_rank == 100) {
				mm_x = x;
				mm_y = y;
				mm_set = 1;
				origin = "i915_display_info";
			}
		}
		if (c && strstr(line, "hdisp")) {
			int x, y;
			while ((*c == ' ') || (*c == ':') || (*c == '"')) c++;

			x = strtoull(c, NULL, 10);
			if (!x)
				break;
			c = strchr(c, 'x');
			if (!c)
				break;
			c++;
			y = strtoull(c, NULL, 10);
			if (x && y && current_rank == 100) {
				pixels_x = x;
				pixels_y = y;
				origin = "i915_display_info";
			}
		}
	}
	fclose(file);
}
static void parse_framebuffer_data(void)
{
	int fd;
	struct fb_var_screeninfo vinfo;


	static const char *name = "/dev/__fb0__";
	mknod(name, S_IFCHR | 0600, (29 << 8) | 0);

	fd = open("/dev/graphics/fb0", O_RDWR);

	if (fd < 1)
		fd = open("/dev/fb0", O_RDWR);
	if (fd < 1)
		fd = open("/dev/__fb0__", O_RDWR);

	unlink("/dev/__fb0__");
	if (fd < 0) {
		ERROR("edid: cannot open framebuffer device\n");
		return;
	}

	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
	{
		close(fd);
		ERROR("edid: FBIOGET_VSCREENINFO failed\n");
		return;
	}
	close(fd);

	if (vinfo.xres > 0 && vinfo.yres > 0) {
		ERROR("edid: setting resolution based on framebuffer data\n");
		pixels_x = vinfo.xres;
		pixels_y = vinfo.yres;
		origin = "framebuffer";
		if ((int)vinfo.width > 0 && (int)vinfo.height > 0) {
			mm_x = vinfo.width;
			mm_y = vinfo.height;
			mm_set = 1;
			origin = "framebuffer physical dimensions";
		}
	}
}

static int get_edid_dpi(void)
{
	DIR *dir;
	char path[PATH_MAX];
	struct dirent *entry;

	ERROR("edid: start get_edid_dpi\n");

	parse_framebuffer_data();
	parse_display_info("/sys/kernel/debug/dri/0/i915_display_info");


	dir = opendir("/sys/class/drm/");
	if (!dir)
		return dpi;
	while (1) {
		entry = readdir(dir);
		if (!entry)
			break;

		if (entry->d_name[0] == '.')
			continue;

		sprintf(path, "/sys/class/drm/%s/edid", entry->d_name);

		parse_edid(path);

	}
	closedir(dir);

	dpi_math();

	if (dpi > 0) {
		sprintf(path, "%i", dpi);
		ERROR("edid: Setting DPI property to %s\n", path);
		property_set("ro.sf.lcd_density", path);
		if (origin)
			property_set("ro.sf.lcd_density_origin", origin);
		sprintf(path, "%i x %ipx %imm x %imm  %i dpi => density: %i", pixels_x, pixels_y, mm_x, mm_y, rawdpi, dpi);
		property_set("ro.sf.lcd_density_info", path);

	}

	return dpi;
}




/* Since we run as init and early, we can't actually do a property_get (which creates recursion),
 * so we need to cache the key properties.
 *
 * NOTE: the order of brand/name/device is special and needs to be 0/1/2
 */
#define _PROP_BRAND 0
#define _PROP_NAME 1
#define _PROP_DEVICE 2
#define _PROP_BOOTLOADER 3
#define _PROP_SERIAL 4
#define _PROP_MODEL 5
static char cached_properties[6][PROP_VALUE_MAX];

static void CDD_clean_string(char *buf)
{
	char *c;
	/* insure the string conforms with CDD v4.4 section 3.2.2
	 * which requires matching the regexp "^[a-zA-Z0-9.,_-]+$",
	 * but disallow '.' which Google has confirmed should not be
	 * allowed in at least the device build fingerprint prefix
	 * and thus by paranoia we fall back to removing it everywhere */

	c = buf;
	while (*c) {
		if ( (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <='9') || (*c == ',') || (*c == '_') || (*c == '-')) {
			/* Google prefers lower case */
			*c = tolower(*c);
			/* valid character */
		} else {
			*c = '_';
		}

		c++;
	}

	while (strlen(buf) > 0 && (buf[strlen(buf) - 1] == '_' || buf[strlen(buf) - 1] == '.'))
			 buf[strlen(buf) - 1] = 0;

}

/* Remove any trailing "_inc*", "_corp*", "_gmbh*".
 * Force set some known-to-misbehave brands names to a good form */
static void chop_brand_tail(void)
{
	char *c = NULL;

	if (strncasecmp(cached_properties[_PROP_BRAND], "intel", 5) == 0) {
		strncpy(cached_properties[_PROP_BRAND], "intel\0", 6);
		return;
	}

	if (strncasecmp(cached_properties[_PROP_BRAND], "asus", 4) == 0) {
		strncpy(cached_properties[_PROP_BRAND], "asus\0", 5);
		return;
	}

	c = strcasestr(cached_properties[_PROP_BRAND], "_inc");
	if (c) {
		*c = 0;
		return;
	}

	c = strcasestr(cached_properties[_PROP_BRAND], "_corp");
	if (c) {
		*c = 0;
		return;
	}

	c = strcasestr(cached_properties[_PROP_BRAND], "_gmbh");
	if (c) {
		*c = 0;
		return;
	}
}

/* Check input for what we would consider a valid serial number.
 * On any invocation, if the input doesn't appear good, we return
 * modify the input to be a null string. */
static void CDD_clean_serialno(char *buf)
{
	char *c;
	unsigned int zeros = 0;

	/* CDD version v4.4 section 3.2.2 indicates a serial number must
	 * match the regexp "^([a-zA-Z0-9]{6,20})$" */

	/* basic IQ test for BIOS s/n: */
	if ((strcasestr(buf, "serial") != NULL) ||	/* matches: "System Serial Number" */
	    (strcasestr(buf, "filled") != NULL) ||	/* matches: "To be filled by O.E.M" */
	    (strcasestr(buf, "12345678") != NULL)) {	/* matches: common non-random number */
		buf[0] = 0;
		return;
	}

	if (strlen(buf) < 6) {				/* force CDD min length compliance */
		buf[0] = 0;
		return;
	}

	c = buf;
	while (*c) {
		if ( (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <='9')) {
			/* valid character */
		} else {				/* force non-CDD compliant chars to zero */
			*c = '0';
		}

		if (*c == '0')
			zeros++;

		c++;
	}

	if (strlen(buf) == zeros) {			/* serial number was all zeros...bogus */
		buf[0] = 0;
		return;
	}

	if (strlen(buf) > 20) {				/* force CDD max length compliance */
		buf[20] = '\0';
	}
}

/* After the repeated calls to set the serial number, hopefully one has
 * stuck.  If not, we set a canary so the new device and its firmware can't
 * get a good CTS result and can't get into the GOTA stream until we
 * address its firmware problems. */
static void check_serialno(void)
{
	if (strlen(cached_properties[_PROP_SERIAL]) == 0) {
		strncpy(cached_properties[_PROP_SERIAL], "00badbios00badbios00\0", 21);
	}
}

/* Callers will make multiple calls to this function in succession.  The
 * first call to find valid data will populate the found and cleaned data
 * into cached_properties[prop_type], otherwise that array is left unchanged. */
static void get_property_from_dmi_file(const char *dmi_name, const char *propname, int prop_type)
{
	FILE *file;
	char filename[PATH_MAX];
	char buf[PROP_VALUE_MAX];
	char *c;

	if (strlen(cached_properties[prop_type]) != 0)
		return;

	sprintf(filename, "/sys/devices/virtual/dmi/id/%s", dmi_name);
	file = fopen(filename, "r");
	if (!file)
		return;

	memset(buf, 0, PROP_VALUE_MAX);
	if (fgets(buf, PROP_VALUE_MAX, file) == NULL) {
		fclose(file);
		return;
	}

	fclose(file);
	c = strchr(buf, '\n');
	if (c)
		*c = 0;

	if (prop_type == _PROP_SERIAL) {
		CDD_clean_serialno(buf);
	} else {
		CDD_clean_string(buf);
	}

	if (strlen(buf) > 0)
		strncpy(cached_properties[prop_type], buf, PROP_VALUE_MAX);
}

/* ro.product.device aka _PROP_DEVICE
 *   and
 * ro.product.name aka _PROP_NAME
 *
 * are special cases:
 *
 * We want the ability to do something sane enough on any new hardware, but for
 * known/supported hardware we actually need a short, consistent, well-known
 * string not something arbitrary, because shorten_fingerprint() MUST NOT
 * truncate for example the boardversion off the back of the field or
 * worst case devices will be bricked in the field.
 *
 * Therefore we start out with something like a table lookup, else
 * speculatively go with whatever DMI gave us and shorten it.
 *
 * Problems to watch for:
 *  - board_version varies with flashed BIOS (not allowed)
 *  - product_name / board_name are long
 *  - product_version / board_version are long
 */
static void get_property_DEVICE(void)
{
	FILE *file;
	char buf[PROP_VALUE_MAX];
	char boardname[PROP_VALUE_MAX];
	char boardversion[PROP_VALUE_MAX];
	char *c;

	file = fopen("/sys/devices/virtual/dmi/id/board_name", "r");
	if (!file)
		return;

	memset(boardname, 0, PROP_VALUE_MAX);
	if (fgets(boardname, PROP_VALUE_MAX, file) == NULL) {
		fclose(file);
		return;
	}

	fclose(file);
	c = strchr(boardname, '\n');
	if (c)
		*c = 0;

	CDD_clean_string(boardname);

	if (strlen(boardname) == 0)
		return;

	memset(boardversion, 0, PROP_VALUE_MAX);
	file = fopen("/sys/devices/virtual/dmi/id/board_version", "r");
	if (file) {
		if (fgets(boardversion, PROP_VALUE_MAX, file) == NULL)
			boardversion[0] = 0;

		fclose(file);
		c = strchr(boardversion, '\n');
		if (c)
			*c = 0;

		CDD_clean_string(boardversion);

		/* treat first final revision boards as versionless to keep
		 * fingerprint length shorter */
		if (strncmp(boardversion, "1_0", 3) == 0) {
			boardversion[0] = 0;
		}
	}

	/* may want a table here, eg:  if device==XX then device:=YY */

#define IRDA_FISHNAME "coho"
	if (boardversion[0] == 0) {
		sprintf(buf, "%s_%s", boardname, IRDA_FISHNAME);
	} else {
		sprintf(buf, "%s_%s_%s", boardname, boardversion, IRDA_FISHNAME);
	}

	strncpy(cached_properties[_PROP_DEVICE], buf, PROP_VALUE_MAX);
}

/* board_name observed to often be "ugly" or outright bad
 * product_name observed to usually be ok */
static void get_property_NAME(void)
{
	get_property_from_dmi_file("product_name", "ro.product.name", _PROP_NAME);
	get_property_from_dmi_file("board_name", "ro.product.name", _PROP_NAME);

	/* may want a table here:  if name==XX then name:=YY */
}

/* product_vendor probably doesn't exist
 * sys_vendor observed to be blank on some devices
 * bios_vendor will be different than what we want here (DO NOT USE IT)
 * board_vendor observed to be reasonable on sample of devices */
static void get_property_BRAND(void)
{
	get_property_from_dmi_file("board_vendor", "ro.product.brand", _PROP_BRAND);
	get_property_from_dmi_file("sys_vendor", "ro.product.brand", _PROP_BRAND);
	get_property_from_dmi_file("product_vendor", "ro.product.brand", _PROP_BRAND);

	/* Google request: remove trailing Inc and such */
	chop_brand_tail();
}

static void get_property_BOOTLOADER(void)
{
	get_property_from_dmi_file("bios_version", "ro.bootloader", _PROP_BOOTLOADER);

	if (strlen(cached_properties[_PROP_BOOTLOADER]) > 0) {
		property_set("ro.bootloader", cached_properties[_PROP_BOOTLOADER]);
		property_set("ro.boot.bootloader", cached_properties[_PROP_BOOTLOADER]);
	}
}

static void get_property_SERIAL(void)
{
    /*    product_uuid is the only field observed to be filled in across many
     *    devices and vendors, but the other fields likely hold the "real"
     *    serial number printed on packaging and/or used by other components
     *    (eg: ADB/USB, recovery console, fastboot, bootloader, BIOS, etc.)
     *
     *    product_uuid is observed to be a standard 16-octet 128bit UUID
     *
     *    the other fields are observed to be truncated/shorter numbers
     */
    get_property_from_dmi_file("product_serial", "ro.serialno", _PROP_SERIAL);
    get_property_from_dmi_file("chassis_serial", "ro.serialno", _PROP_SERIAL);
    get_property_from_dmi_file("board_serial", "ro.serialno", _PROP_SERIAL);
    get_property_from_dmi_file("product_asset_tag", "ro.serialno", _PROP_SERIAL);
    get_property_from_dmi_file("chassis_asset_tag", "ro.serialno", _PROP_SERIAL);
    get_property_from_dmi_file("board_asset_tag", "ro.serialno", _PROP_SERIAL);
    get_property_from_dmi_file("product_uuid", "ro.serialno", _PROP_SERIAL);

    check_serialno();

    if (strlen(cached_properties[_PROP_SERIAL]) > 0) {
		property_set("ro.serialno", cached_properties[_PROP_SERIAL]);
		property_set("ro.boot.serialno", cached_properties[_PROP_SERIAL]);
    }
}

static void get_property_MODEL(void)
{
	/* CDD leaves the ro.product.model field free form but it must not
	 * be empty.  IRDA is working with IBV's to have them put their
	 * human-pretty marketing name in the DMI field "board_name1".  If
	 * that non-standard DMI field is empty, the IBV messed up.  In
	 * that case just fall back to _PROP_DEVICE, which was set by an
	 * earlier call to get_property_DEVICE().
	 */
	FILE *file;
	char buf[PROP_VALUE_MAX];
	char *c;

	file = fopen("/sys/devices/virtual/dmi/id/board_name1", "r");
	if (!file)
		goto fallback;

	memset(buf, 0, PROP_VALUE_MAX);
	if (fgets(buf, PROP_VALUE_MAX, file) == NULL) {
		fclose(file);
		goto fallback;
	}

	fclose(file);
	c = strchr(buf, '\n');
	if (c)
		*c = 0;

	if (strlen(buf) != 0) {
		/* use the IBV's "board_name1" value */
		strncpy(cached_properties[_PROP_MODEL], buf, PROP_VALUE_MAX);
		return;
	}

fallback:
	/* fallback to value previously set by get_property_DEVICE() */
	strncpy(cached_properties[_PROP_MODEL], cached_properties[_PROP_DEVICE], PROP_VALUE_MAX);
}

static void load_properties_from_dmi(void)
{
    /* Read vendor from DMI and sanitize */
    get_property_BRAND();

    /* Craft a product name with influence from DMI data */
    get_property_NAME();

    /* Craft a device name with influence from DMI data */
    get_property_DEVICE();

    /* Read firmware version from DMI */
    get_property_BOOTLOADER();

    /* Seek out and sanitize serial number */
    get_property_SERIAL();

    /* Read and sanitize OEM's model string */
    get_property_MODEL();
}

/* NOTE: this function should only be called on a non-qualified BIOS instance
 * which presents bad DMI information.  */
static void shorten_fingerprint(void)
{
	int i;
	while (strlen(cached_properties[_PROP_BRAND]) + strlen(cached_properties[_PROP_NAME]) + strlen(cached_properties[_PROP_DEVICE]) > 33) {
		for (i = _PROP_DEVICE ; i >= _PROP_BRAND; i--) {
			strcpy(cached_properties[i], "ERROR");
		}
	}
}

/* CDD says: $(BRAND)/$(PRODUCT)/$(DEVICE):$(VERSION.RELEASE)/$(ID)/$(VERSION.INCREMENTAL):$(TYPE)/$(TAGS)
 * from /system/build.prop we get strings like:
 *      ro.build.fingerprint=generic/starpeak/starpeak:4.4.2/768/eng.arjan.20140506.154354:userdebug/test-keys
 *      ro.build.fingerprint=Android/irda/irda:4.4.4/KTU84P/IRDA00150:userdebug/test-keys
 * Cut out until the ':', replacing with brand/product/device from DMI properties, then set the property.  */
static void create_fingerprint(void)
{
	char fingerprint[4 * PROP_VALUE_MAX];
	char *c;
	int i;
	FILE *file;
	char line[PATH_MAX];
	char original[PATH_MAX];
	char *clientid = NULL;
	char *s = NULL;
	original[0] = 0;

	/* we are usually in the Android OS */
	file = fopen("/system/build.prop", "r");
	if (!file) {
		/* then we are likely in the Recovery Console */
		file = fopen("/default.prop", "r");
		if (!file)
			return;
	}

	while (!feof(file)) {
		memset(line, 0, PATH_MAX);
		if (fgets(line, PATH_MAX, file) == 0)
			break;

		c = strchr(line, '=');
		if (strstr(line, "ro.build.fingerprint=") && c) {
			c++;
			strcpy(original, c);
			c = strchr(original, '\n');
			if (c)
				*c = 0;
		}
	}
	fclose(file);

	for (i = 0; i < 3; i++)
		if (strlen(cached_properties[i]) == 0)
			strcpy(cached_properties[i], "BIOSBUG");


	c = strchr(original, ':');
	if (!c)
		return;

	if (strlen(cached_properties[_PROP_BRAND]) + strlen(cached_properties[_PROP_NAME]) + strlen(cached_properties[_PROP_DEVICE]) + strlen(c) + 3 > 91)
		shorten_fingerprint();

	property_set("ro.product.brand", cached_properties[_PROP_BRAND]);

	property_set("ro.product.name", cached_properties[_PROP_NAME]);

	property_set("ro.product.device", cached_properties[_PROP_DEVICE]);
	property_set("ro.build.product", cached_properties[_PROP_DEVICE]);
	property_set("ro.product.board", cached_properties[_PROP_DEVICE]);
	property_set("ro.board.platform", cached_properties[_PROP_DEVICE]);

	sprintf(fingerprint, "%s/%s/%s%s", cached_properties[_PROP_BRAND], cached_properties[_PROP_NAME], cached_properties[_PROP_DEVICE], c);
	property_set("ro.build.fingerprint", fingerprint);

	property_set("ro.product.model", cached_properties[_PROP_MODEL]);

	if(!efivar_get_google_clientid(clientid)) {
		asprintf(&s, "android-%s", clientid);
		free(clientid);
	} else {
		asprintf(&s, "android-%s", cached_properties[_PROP_BRAND]);
	}
	property_set("ro.com.google.clientidbase", s);
	free(s);
}



void autodetect_properties(void)
{
	load_properties_from_dmi();
	create_fingerprint();

	get_edid_dpi();
}


char boardname[4096];

/* DMI board name */
static void read_board_name(void)
{
	FILE *file;
	file = fopen("/sys/class/dmi/id/board_name", "r");
	if (!file)
		return;
	if (!fgets(boardname, 4095, file))
		ERROR("Failed to read boardname\n");
	fclose(file);
}


/* sysfs helpers */
static void write_int_to_file(char *filename, int value)
{
	FILE *file;

	file = fopen(filename, "w");
	if (!file) {
		ERROR("Cannot write %i to %s: %s\n",
		      value, filename, strerror(errno));
		return;
	}

	fprintf(file, "%i\n", value);
	fclose(file);
}


static void write_string_to_file(char *filename, char *string)
{
	FILE *file;

	file = fopen(filename, "w");
	if (!file) {
		ERROR("INIT Cannot write %s to %s: %s\n",
		      string, filename, strerror(errno));
		return;
	}

	fprintf(file, "%s\n", string);
	fclose(file);
}

/* SATA links */
static void do_sata_links(void)
{
	DIR *dir;
	struct dirent *entry;

	dir = opendir("/sys/class/scsi_host");
	if (!dir)
		return;

	do {
		char *filename;

		entry = readdir(dir);
		if (!entry)
			break;

		if (strcmp(entry->d_name, ".") == 0)
			continue;
		if (strcmp(entry->d_name, "..") == 0)
			continue;

		if (asprintf(&filename,
			     "/sys/class/scsi_host/%s/link_power_management_policy",
			     entry->d_name) < 0)
			return;

		write_string_to_file(filename, "min_power");
		free(filename);

	} while (1);

	closedir(dir);
}

/* Virtual Memory tweaks */
static void do_vm_tweaks(void)
{
	/* synchronous dirty ratio --> 50% */
	write_int_to_file("/proc/sys/vm/dirty_ratio", 50);
	/*
	 * start IO at 30% not 10%...
	 * the FS/timeout based write generates better IO patterns
	 */
	write_int_to_file("/proc/sys/vm/dirty_background_ratio", 30);
	/*
	 * 15 seconds before the VM starts writeback,
	 * allowing the FS to deal with this better
	 */
	write_int_to_file("/proc/sys/vm/dirty_writeback_centisecs", 1500);
	write_int_to_file("/sys/kernel/mm/transparent_hugepage/khugepaged/scan_sleep_millisecs",
			  300000);

	write_int_to_file("/sys/block/sda/queue/nr_requests", 4096);

	 /* android can't cope with more than 32k */
	write_int_to_file("/proc/sys/vm/mmap_min_addr", 32 * 1024);

	/* oom less */
	write_int_to_file("/proc/sys/vm/extfrag_threshold", 100);
	write_int_to_file("/sys/kernel/mm/ksm/sleep_millisecs", 10000);
	write_int_to_file("/sys/kernel/mm/ksm/run", 1);
	write_int_to_file("/sys/kernel/mm/ksm/pages_to_scan", 1000);
}

/* NMI watch dog */
static void do_nmi_watchdog(void)
{
	write_int_to_file("/proc/sys/kernel/nmi_watchdog", 0);
}

/* Audio PM */
static void do_audio(void)
{
	write_int_to_file("/sys/module/snd_hda_intel/parameters/power_save", 1);
}

/* P-state */
static void do_pstate(void)
{
	write_string_to_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
			     "powersave");
	/*
	 * we want at least half performance, this helps us in race-to-halt
	 * and to give us reasonable responses
	 */
	write_int_to_file("/sys/devices/system/cpu/intel_pstate/min_perf_pct", 50);
}

static int pnp_init(void)
{
	read_board_name();

	/* SATA link power management -- except on preproduction hardware */
	if (!strcmp(boardname, "NOTEBOOK\n"))
		do_sata_links();

	/* VM writeback timeout and dirty pages */
	do_vm_tweaks();

	/* turn off the NMI wathdog */
	do_nmi_watchdog();

	/* Audio PM */
	do_audio();

	/* P-state */
	do_pstate();

	return 0;
}

#ifndef HAL_AUTODETECT_KMSG_NAME
#define HAL_AUTODETECT_KMSG_NAME "/dev/__hal_kmsg__"
#endif

void autodetect_init(void)
{
	int rc;

	/*
	 * Create a klog node for hald
	 * Because os sepolicy constraints, hald cannot use mknod. So it's
	 * created by init and opened by hald.
	 */
	if (mknod(HAL_AUTODETECT_KMSG_NAME, S_IFCHR | 0600, (1 << 8) | 11) < 0)
		ERROR("Could not create '%s' character device: %s\n",
		      HAL_AUTODETECT_KMSG_NAME, strerror(errno));

	pnp_init();
}
