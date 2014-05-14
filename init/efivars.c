/*
 *   StarPeak derived EFI variable reader used to allow the IRDA OS or firmware
 *   to influence which Android ro.properties are set to what dynamic values.
 *
 *      Copyright © 2014 Intel Corporation.
 *
 *   Authors:
 *         Christophe Guiraud <christophe.guiraud@intel.com>
 *         Tim Pepper <timothy.c.pepper@intel.com>
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include "log.h"


typedef struct efi_guid_t {
	uint8_t  b[16];
} efi_guid_t;

#define EFI_GUID(a, b ,c ,d0 ,d1 ,d2 ,d3 ,d4 ,d5 ,d6 ,d7) \
	((efi_guid_t) \
	{{(a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
	  (b) & 0xff, ((b) >> 8) & 0xff, \
	  (c) & 0xff, ((c) >> 8) & 0xff, \
	  (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7)}})

#define GUID_FORMAT \
	"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x"

#define EFI_VARIABLE_NON_VOLATILE	0x0000000000000001
#define EFI_VARIABLE_BOOTSERVICE_ACCESS	0x0000000000000002
#define EFI_VARIABLE_RUNTIME_ACCESS	0x0000000000000004

#define RO_PROP_VAR_ATTRIBUTES \
		EFI_VARIABLE_RUNTIME_ACCESS | \
		EFI_VARIABLE_BOOTSERVICE_ACCESS | \
		EFI_VARIABLE_NON_VOLATILE

#define VARS_PATH "/sys/firmware/efi/vars/"

/* ro.properties GUID == f87b0c71-ff01-cb2e-ba47-5308e996bd0f */
static const efi_guid_t RO_PROPERTIES_GUID =
	EFI_GUID(0xf87b0c71, 0xff01, 0xcb2e, 0xba, 0x47, 0x53, 0x08, 0xe9, 0x96, 0xbd, 0x0f);

/* ro.com.google.clientidbase == GoogleClientID */
#define RO_PROP_GOOGLE_CLIENTID_VAR "GoogleClientID"

typedef struct efi_variable_32_t {
	uint16_t	name[1024 / sizeof(uint16_t)];
	efi_guid_t	guid;
	uint32_t	data_sz;
	uint8_t		data[1024];
	uint32_t	status;
	uint32_t	attributes;
} __attribute__((packed)) efi_variable_32_t;


typedef struct efi_variable_64_t {
	uint16_t	name[1024 / sizeof(uint16_t)];
	efi_guid_t	guid;
	uint64_t	data_sz;
	uint8_t		data[1024];
	uint64_t	status;
	uint32_t	attributes;
} __attribute__((packed)) efi_variable_64_t;


static int kernel_arch_64_bit(bool *is_64bit)
{
	char version[PATH_MAX];
	FILE *file;
	char *tmp1, *tmp2;
	const char *prefix = "Linux version ";

	file = fopen("/proc/version", "r");
	if (file == NULL)
		return -1;

	tmp1 = fgets(version, PATH_MAX, file);
	fclose(file);
	if (tmp1 == NULL)
		return -1;

	tmp1 = strstr(version, prefix);
	if (tmp1 == NULL)
		return -1;

	tmp2 = strchr(tmp1 + strlen(prefix), ' ');
	if (tmp2)
		*tmp2 = '\0';

	if (strstr(tmp1, "x86_64") != NULL)
		*is_64bit = true;
	else
		*is_64bit = false;

	return 0;
}

static uint16_t *char_str_to_efi_str(const char *src)
{
	size_t i = 0;
	uint16_t *dst;
	size_t src_len = strlen(src);

	dst = calloc(src_len + 1, sizeof(uint16_t));
	if (!dst)
		abort();

	while (i < src_len) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = 0;

	return dst;
}

static size_t efi_str_length(const uint16_t *str)
{
	size_t len = 0;

	while (*(str + len))
		len++;

	return len;
}

static char *efi_str_to_char_str(const uint16_t *src)
{
	size_t i = 0;
	char *dst;
	size_t src_len = efi_str_length(src);

	dst = calloc(src_len + 1, sizeof(char));
	if (!dst)
		abort();

	while (i < src_len) {
		dst[i] = src[i];
		i++;
	}
	dst[i] = '\0';

	return dst;
}

static char *get_efi_path(efi_guid_t guid, const char *name, const char *entry)
{
	char *path;

	if (asprintf(&path, VARS_PATH "%s-" GUID_FORMAT "%s", name,
			  guid.b[3], guid.b[2], guid.b[1], guid.b[0],
			  guid.b[5], guid.b[4], guid.b[7], guid.b[6],
			  guid.b[8], guid.b[9], guid.b[10], guid.b[11],
			  guid.b[12], guid.b[13], guid.b[14], guid.b[15],
			  entry) <= 0)
		abort();

	return path;
}

static int efi_write(const char *entry, void *data, ssize_t len)
{
	int fd;

	fd = open(entry, O_WRONLY);
	if (fd < 0) {
		ERROR("efivars: Failed to open file entry=%s, strerror=%s\n",
				entry, strerror(errno));
		return -1;
	}

	if (write(fd, data, len) != len) {
		ERROR("efivars: Failed to write to file entry=%s, strerror=%s\n",
				entry, strerror(errno));
		close(fd);
		return -1;
	}

	close(fd);

	return 0;
}

static bool efi_variable_exists(efi_guid_t guid, const char *name)
{
	char *path;
	bool result = false;

	path = get_efi_path(guid, name, "");

	if (access(path, F_OK) == 0)
		result = true;

	free(path);

	return result;
}

static int read_efi_variable(efi_guid_t guid, const char *name,
			     void **raw_var, ssize_t *raw_var_len,
			     bool *is_64bit)
{
	int fd;
	char *path;
	efi_variable_32_t *efi_var_32 = NULL;
	efi_variable_64_t *efi_var_64 = NULL;
	bool kernel_64_bit;

	if (kernel_arch_64_bit(&kernel_64_bit) < 0) {
		ERROR("efivars: kernel architecture detection failed\n");
		return -1;
	}

	path = get_efi_path(guid, name, "/raw_var");
	if (path == NULL)
		return -1;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		ERROR("efivars: Failed to open file path=%s, strerror=%s\n",
				path, strerror(errno));
		free(path);
		return -1;
	}

	if (kernel_64_bit) {
		efi_var_64 = malloc(sizeof(efi_variable_64_t));
		if (!efi_var_64) {
			close(fd);
			free(path);
			return -1;
		}
		*raw_var = efi_var_64;
		*raw_var_len = sizeof(efi_variable_64_t);
	} else {
		efi_var_32 = malloc(sizeof(efi_variable_32_t));
		if (!efi_var_32) {
			close(fd);
			free(path);
			return -1;
		}
		*raw_var = efi_var_32;
		*raw_var_len = sizeof(efi_variable_32_t);
	}

	if (read(fd, *raw_var, *raw_var_len) != *raw_var_len) {
		ERROR("efivars: Failed to read file path=%s, strerror=%s\n",
				path, strerror(errno));
		close(fd);
		free(*raw_var);
		*raw_var = NULL;
		free(path);
		return -1;
	}

	if (is_64bit != NULL)
		*is_64bit = kernel_64_bit;

	close(fd);
	free(path);

	return 0;
}

static int get_efi_variable(efi_guid_t guid, const char *name, char **value)
{
	int ret;
	void *raw_var = NULL;
	ssize_t raw_var_len = 0;
	bool kernel_64_bit;
	const uint16_t *data;
	efi_variable_32_t *efi_var_32_ptr;
	efi_variable_64_t *efi_var_64_ptr;

	*value = NULL;

	ret = read_efi_variable(guid, name, &raw_var, &raw_var_len,
				&kernel_64_bit);
	if (ret < 0)
		return -1;

	if (kernel_64_bit) {
		efi_var_64_ptr = (efi_variable_64_t*)raw_var;
		data = (const uint16_t *)&(efi_var_64_ptr->data);
	} else {
		efi_var_32_ptr = (efi_variable_32_t*)raw_var;
		data = (const uint16_t *)&(efi_var_32_ptr->data);
	}

	*value = efi_str_to_char_str(data);

	free(raw_var);
	return 0;
}

static int delete_efi_variable(efi_guid_t guid, const char *name)
{
	int ret;
	void *raw_var = NULL;
	ssize_t raw_var_len = 0;

	ret = read_efi_variable(guid, name, &raw_var, &raw_var_len, NULL);
	if (ret < 0)
		return -1;

	ret = efi_write(VARS_PATH "del_var", raw_var, raw_var_len);

	free(raw_var);
	return ret;
}

static int set_efi_variable(efi_guid_t guid, const char *name,
			    const char *value)
{
	int i;
	char *path;
	void *raw_var = NULL;
	ssize_t raw_var_len = 0;
	bool kernel_64_bit;
	efi_variable_32_t efi_var_32;
	efi_variable_64_t efi_var_64;
	size_t efi_value_size;
	uint16_t *efi_value;

	if (kernel_arch_64_bit(&kernel_64_bit) < 0) {
		ERROR("efivars: kernel architecture detection failed\n");
		return -1;
	}

	efi_value = char_str_to_efi_str(value);
	efi_value_size = ((efi_str_length(efi_value) + 1) * sizeof(uint16_t));

	if ((strlen(name) > 1024) || (efi_value_size > 1024)) {
		ERROR("efivars: Invalid EFI variable parameter\n");
		free(efi_value);
		return -1;
	}

	path = get_efi_path(guid, name, "/data");
	if (path == NULL) {
		free(efi_value);
		return -1;
	}

	if ((access(path, F_OK) == 0) &&
	    (delete_efi_variable(guid, name) < 0)) {
		free(efi_value);
		free(path);
		return -1;
	}

	free(path);

	if (kernel_64_bit) {
		memset(&efi_var_64, 0, sizeof(efi_var_64));
		efi_var_64.guid = guid;
		efi_var_64.status = 0;
		efi_var_64.attributes = (uint32_t)RO_PROP_VAR_ATTRIBUTES;
		for (i = 0; name[i] != '\0'; i++)
			efi_var_64.name[i] = name[i];
		efi_var_64.data_sz = efi_value_size;
		memcpy(efi_var_64.data, (void *)efi_value, efi_value_size);
		raw_var = &efi_var_64;
		raw_var_len = sizeof(efi_var_64);
	} else {
		memset(&efi_var_32, 0, sizeof(efi_var_32));
		efi_var_32.guid = guid;
		efi_var_32.status = 0;
		efi_var_32.attributes = (uint32_t)RO_PROP_VAR_ATTRIBUTES;
		for (i = 0; name[i] != '\0'; i++)
			efi_var_32.name[i] = name[i];
		efi_var_32.data_sz = efi_value_size;
		memcpy(efi_var_32.data, (void *)efi_value, efi_value_size);
		raw_var = &efi_var_32;
		raw_var_len = sizeof(efi_var_32);
	}

	free(efi_value);

	if (efi_write(VARS_PATH "new_var", raw_var, raw_var_len) < 0)
		return -1;

	return 0;
}

static void dump_efi_var(const efi_guid_t guid, const char *name)
{
	int ret;
	char *value = NULL;

	if (!efi_variable_exists(guid, name)) {
		ERROR("efivars: [%s] EFI variable doesn't exist\n", name);
	} else {
		ret = get_efi_variable(guid, name, &value);
		if ((ret < 0) || (value == NULL)) {
			ERROR("efivars: [%s] Failed to retrieve EFI variable: %s\n", name, strerror(errno));
			return;
		}
		ERROR("efivars: [%s] = %s\n", name, value);
		free(value);
	}
}

/******************************************************************************/

int efivar_get_google_clientid(char *str)
{
	int ret = 0;

	ret = efi_variable_exists(RO_PROPERTIES_GUID, RO_PROP_GOOGLE_CLIENTID_VAR);
	if (ret == 0) {
		ERROR("efivars: [" RO_PROP_GOOGLE_CLIENTID_VAR "] EFI variable doesn't exist\n");
		return -1;
	}

	ret = get_efi_variable(RO_PROPERTIES_GUID, RO_PROP_GOOGLE_CLIENTID_VAR, &str);
	if ((ret < 0) || (str == NULL)) {
		ERROR("efivars: [" RO_PROP_GOOGLE_CLIENTID_VAR "] Failed to retrieve EFI variable: %s\n", strerror(errno));
		return ret;
	}

	return 0;
}

int efivar_clear_google_clientid(void)
{
	int ret = 0;
	char *value = NULL;

	ret = efi_variable_exists(RO_PROPERTIES_GUID, RO_PROP_GOOGLE_CLIENTID_VAR);
	if (ret == 0) {
		ERROR("efivars: [" RO_PROP_GOOGLE_CLIENTID_VAR "] EFI variable doesn't exist\n");
		return ret;
	}

	ret = get_efi_variable(RO_PROPERTIES_GUID, RO_PROP_GOOGLE_CLIENTID_VAR, &value);
	if ((ret < 0) || (value == NULL)) {
		ERROR("efivars: [" RO_PROP_GOOGLE_CLIENTID_VAR "] Failed to retrieve EFI variable, strerror=%s\n", strerror(errno));
		return ret;
	}

	INFO("efivars: [" RO_PROP_GOOGLE_CLIENTID_VAR "] Delete EFI variable\n");

	ret = delete_efi_variable(RO_PROPERTIES_GUID, RO_PROP_GOOGLE_CLIENTID_VAR);
	if (ret < 0) {
		ERROR("efivars: [" RO_PROP_GOOGLE_CLIENTID_VAR "] Failed to delete EFI variable, strerror=%s\n", strerror(errno));
	}

	return ret;
}
