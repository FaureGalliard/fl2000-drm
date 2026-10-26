/* SPDX-License-Identifier: GPL-2.0 */
/*
 * (C) Copyright 2019, Artem Mygaiev
 */

#ifndef FL2000_IT66121_H
#define FL2000_IT66121_H

struct i2c_adapter;
struct i2c_client;

struct i2c_client *it66121_create(struct i2c_adapter *adapter);
void it66121_destroy(struct i2c_client *client);

#endif /* FL2000_IT66121_H */
