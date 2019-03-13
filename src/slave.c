/*
 * This file is part of the KNOT Project
 *
 * Copyright (c) 2019, CESAR. All rights reserved.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <ell/ell.h>

#include "dbus.h"
#include "source.h"
#include "slave.h"

#define SLAVE_IFACE		"br.org.cesar.modbus.Slave1"

struct slave {
	int refs;
	uint8_t id;
	char *name;
	char *path;
};

static struct l_settings *settings;

static void slave_free(struct slave *slave)
{
	l_free(slave->name);
	l_free(slave->path);
	l_free(slave);
}

static struct slave *slave_ref(struct slave *slave)
{
	if (unlikely(!slave))
		return NULL;

	__sync_fetch_and_add(&slave->refs, 1);

	return slave;
}

static void slave_unref(struct slave *slave)
{
	if (unlikely(!slave))
		return;

	if (__sync_sub_and_fetch(&slave->refs, 1))
		return;

	slave_free(slave);
}


static void settings_debug(const char *str, void *userdata)
{
        l_info("%s\n", str);
}

static struct l_dbus_message *method_source_add(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	struct slave *slave = user_data;
	struct l_dbus_message *reply;
	struct l_dbus_message_builder *builder;
	struct l_dbus_message_iter dict;
	struct l_dbus_message_iter value;
	const char *opath;
	const char *key = NULL;
	const char *name = NULL;
	const char *type = NULL;
	uint16_t address = 0;
	uint16_t size = 0;

	if (!l_dbus_message_get_arguments(msg, "a{sv}", &dict))
		return dbus_error_invalid_args(msg);

	while (l_dbus_message_iter_next_entry(&dict, &key, &value)) {
		if (strcmp(key, "Name") == 0)
			l_dbus_message_iter_get_variant(&value, "s", &name);
		else if (strcmp(key, "Type") == 0)
			l_dbus_message_iter_get_variant(&value, "s", &type);
		else if (strcmp(key, "Address") == 0)
			l_dbus_message_iter_get_variant(&value, "q", &address);
		else if (strcmp(key, "Size") == 0)
			l_dbus_message_iter_get_variant(&value, "q", &size);
		else
			return dbus_error_invalid_args(msg);
	}

	/* FIXME: validate type */
	if (!name || !type || address == 0 || size == 0)
		return dbus_error_invalid_args(msg);

	/* TODO: Add to storage and create source object */
	opath = source_create(slave->path, name, type, address, size);
	if (!opath)
		return dbus_error_invalid_args(msg);

	/* Add object path to reply message */
	reply = l_dbus_message_new_method_return(msg);
	builder = l_dbus_message_builder_new(reply);
	l_dbus_message_builder_append_basic(builder, 'o', opath);
	l_dbus_message_builder_finalize(builder);
	l_dbus_message_builder_destroy(builder);

	return reply;
}

static struct l_dbus_message *method_source_remove(struct l_dbus *dbus,
						struct l_dbus_message *msg,
						void *user_data)
{
	const char *path;

	if (!l_dbus_message_get_arguments(msg, "o", &path))
		return dbus_error_invalid_args(msg);

	/* TODO: remove from storage and destroy source object */

	return l_dbus_message_new_method_return(msg);
}

static bool property_get_id(struct l_dbus *dbus,
				  struct l_dbus_message *msg,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct slave *slave = user_data;

	l_dbus_message_builder_append_basic(builder, 'y', &slave->id);

	return true;
}

static bool property_get_name(struct l_dbus *dbus,
				  struct l_dbus_message *msg,
				  struct l_dbus_message_builder *builder,
				  void *user_data)
{
	struct slave *slave = user_data;

	l_dbus_message_builder_append_basic(builder, 's', slave->name);

	return true;
}

static struct l_dbus_message *property_set_name(struct l_dbus *dbus,
					 struct l_dbus_message *msg,
					 struct l_dbus_message_iter *new_value,
					 l_dbus_property_complete_cb_t complete,
					 void *user_data)
{
	struct slave *slave = user_data;
	const char *name;

	if (!l_dbus_message_iter_get_variant(new_value, "s", &name))
		return dbus_error_invalid_args(msg);

	l_free(slave->name);
	slave->name = l_strdup(name);

	complete(dbus, msg, NULL);

	return NULL;
}

static void setup_interface(struct l_dbus_interface *interface)
{

	/* Add/Remove sources (a.k.a variables)  */
	l_dbus_interface_method(interface, "AddSource", 0,
				method_source_add,
				"o", "a{sv}", "path", "dict");

	l_dbus_interface_method(interface, "RemoveSource", 0,
				method_source_remove, "", "o", "path");

	if (!l_dbus_interface_property(interface, "Id", 0, "y",
				       property_get_id,
				       NULL))
		l_error("Can't add 'Id' property");

	/* Local name to identify slaves */
	if (!l_dbus_interface_property(interface, "Name", 0, "s",
				       property_get_name,
				       property_set_name))
		l_error("Can't add 'Name' property");
}

const char *slave_create(uint8_t id, const char *name, const char *address)
{
	struct slave *slave;
	char *dpath;

	/* "host:port or /dev/ttyACM0, /dev/ttyUSB0, ..."*/

	/* FIXME: id is not unique across PLCs */

	dpath = l_strdup_printf("/slave_%04x", id);

	slave = l_new(struct slave, 1);
	slave->id = id;
	slave->name = l_strdup(name);

	if (!l_dbus_register_object(dbus_get_bus(),
				    dpath,
				    slave_ref(slave),
				    (l_dbus_destroy_func_t) slave_unref,
				    SLAVE_IFACE, slave,
				    L_DBUS_INTERFACE_PROPERTIES, slave,
				    NULL)) {
		l_error("Can not register: %s", dpath);
		l_free(dpath);
		return NULL;
	}

	slave->path = dpath;

	l_info("New slave(%p): %s", slave, dpath);

	/* FIXME: Identifier is a PTR_TO_INT. Missing hashmap */

	return dpath;
}

void slave_destroy(const char *path)
{
	l_dbus_unregister_object(dbus_get_bus(), path);
}

int slave_start(const char *config_file)
{

	l_info("Starting slave ...");

	settings = l_settings_new();
	if (settings == NULL)
		return -ENOMEM;

	l_settings_set_debug(settings, settings_debug, NULL, NULL);
	if (!l_settings_load_from_file(settings, config_file)) {
		l_settings_free(settings);
		return -EIO;
	}

	if (!l_dbus_register_interface(dbus_get_bus(),
				       SLAVE_IFACE,
				       setup_interface,
				       NULL, false))
		l_error("dbus: unable to register %s", SLAVE_IFACE);

	source_start();

	return 0;
}

void slave_stop(void)
{
	source_stop();
	l_settings_free(settings);
}