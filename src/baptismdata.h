/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Copyright © 2023 Michael Heimpold <mhei@heimpold.de>
 */

#ifndef BAPTISMDATA_H
#define BAPTISMDATA_H

#ifdef __cplusplus
extern "C" {
#endif

/* forward declaration of private context */
struct baptismdata_ctx;

/** @brief Initialize library and load baptism data
 *
 * While the baptism data content is open, it is locked - that means, that
 * no other program using this library can access it. The lock is released
 * when `baptismdata_close` is called.
 *
 * Note: The returned pointer is set to NULL in case of error.
 *
 * @param[out] ctx pointer to allocated structure for baptism data context
 * @return 0 in case of success, else negative value
 */
int baptismdata_open(struct baptismdata_ctx **ctx);

/** @brief Release/free baptism data context
 *
 * Release all allocated resources and release the lock of the given context.
 *
 * @param[in] ctx baptism data context
 */
void baptismdata_close(struct baptismdata_ctx *ctx);

/** @brief Import baptism data from file
 *
 * Read and parses baptism variables from a file. The file must have the format:
 * <variable name>=<value>
 * Comments starting with "#" are allowed.
 *
 * @param[in] ctx baptism data context
 * @param[in] filename path to the file to be imported
 * @return 0 in case of success, else negative value
 */
int baptismdata_load_file(struct baptismdata_ctx *ctx, const char *filename);

/** @brief Flush baptism data to the storage device.
 *
 * Write the baptism data back to the storage.
 *
 * @param[in] ctx baptism data context
 * @return 0 in case of success, else negative value
 */
int baptismdata_store(struct baptismdata_ctx *ctx);

/*
 * Low-level API
 */

/** @brief Set a variable
 *
 * Set a variable. If not present yet, it creates a new variable.
 * Otherwise the existing value is changed, or the variable is
 * deleted if value is NULL.
 *
 * @param[in] ctx baptism data context
 * @param[in] varname variable name
 * @param[in] value new content of the variable, in case this is NULL, the variable is dropped
 * @return 0 in case of success, else negative value
 */
int baptismdata_set_var(struct baptismdata_ctx *ctx, const char *varname, const char *value);

/** @brief Get a variable
 *
 * Return value of a variable as string or NULL if variable does not exists.
 * The returned string must be freed by the caller when not used anymore.
 *
 * @param[in] ctx baptism data context
 * @param[in] varname variable name
 * @return value in case of success, NULL in case of error
 */
char *baptismdata_get_var(struct baptismdata_ctx *ctx, const char *varname);

/** @brief Iterator
 *
 * Return a pointer to an baptism data entry. Used to iterate all variables.
 *
 * @param[in] ctx baptism data context
 * @param[in] next
 * @return pointer to next entry or NULL
 */
void *baptismdata_iterator(struct baptismdata_ctx *ctx, void *next);

/** @brief Accessor to get variable name from a baptism data entry
 *
 * @param[in] baptism data entry element
 * @return pointer to name or NULL
 */
const char *baptismdata_get_name(void *entry);

/** @brief Accessor to get variable value from a baptism data entry
 *
 * @param[in] baptism data entry element
 * @return pointer to name or NULL
 */
const char *baptismdata_get_value(void *entry);

/*
 * High-level API
 */

/** @brief Return the device' serial number
 *
 * Return the serial number of the device or NULL if not (yet) set.
 * The returned string must not be free by the caller but is freed
 * when `baptismdata_close` is called.
 *
 * @param[in] ctx baptism data context
 * @return serial number in case of success, NULL in case of error
 */
char *baptismdata_get_serialno(struct baptismdata_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif /* BAPTISMDATA_H */
