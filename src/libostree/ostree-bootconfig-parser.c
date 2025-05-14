/*
 * Copyright (C) 2013 Colin Walters <walters@verbum.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; either version 2 of the licence or (at
 * your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library. If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include "ostree-bootconfig-parser.h"
#include "otutil.h"

struct _OstreeBootconfigParser
{
  GObject parent_instance;

  gboolean parsed;
  const char *separators;

  guint64 tries_left;
  guint64 tries_done;

  GHashTable *options;

  /* Additional initrds; the primary initrd is in options. */
  char **overlay_initrds;
};

typedef GObjectClass OstreeBootconfigParserClass;

G_DEFINE_TYPE (OstreeBootconfigParser, ostree_bootconfig_parser, G_TYPE_OBJECT)

/**
 * ostree_bootconfig_parser_clone:
 * @self: Bootconfig to clone
 *
 * Returns: (transfer full): Copy of @self
 */
OstreeBootconfigParser *
ostree_bootconfig_parser_clone (OstreeBootconfigParser *self)
{
  OstreeBootconfigParser *parser = ostree_bootconfig_parser_new ();

  GLNX_HASH_TABLE_FOREACH_KV (self->options, const char *, k, const char *, v)
    g_hash_table_replace (parser->options, g_strdup (k), g_strdup (v));

  parser->overlay_initrds = g_strdupv (self->overlay_initrds);

  return parser;
}

/**
 * ostree_bootconfig_parser_parse_tries:
 * @self: Parser
 * @path: File path
 *
 * Parses a suffix of two counters in the form "+LEFT-DONE" from the end of the
 * filename (excluding file extension).
 */
static void
parse_bootloader_tries (const char *filename, guint64 *out_left, guint64 *out_done, GError **error)
{
  gchar *counter = (gchar *) strrchr (filename, '+');
  const gchar *old_counter = NULL;
  *out_left = 0;
  *out_done = 0;

  if (!counter)
    return;

  counter += 1;

  guint64 tries_left, tries_done = 0;

  old_counter = counter;
  tries_left = g_ascii_strtoull (old_counter, &counter, 10);
  if ((old_counter == counter) || (tries_left > INT_MAX))
    return;

  /* Parse done counter only if present */
  if (*counter == '-')
    {
      old_counter = counter;
      tries_done = g_ascii_strtoull (counter + 1, &counter, 10);
      if ((old_counter == counter) || (tries_left > INT_MAX))
        return;
    }

  *out_left = tries_left;
  *out_done = tries_done;
}

/**
 * ostree_bootconfig_parser_get_tries_left:
 * @self: Parser
 *
 * Returns: Amount of boot tries left
 *
 * Since: 2025.2
 */
guint64
ostree_bootconfig_parser_get_tries_left (OstreeBootconfigParser *self)
{
  return self->tries_left;
}

/**
 * ostree_bootconfig_parser_get_tries_done:
 * @self: Parser
 *
 * Returns: Amount of boot tries
 */
guint64
ostree_bootconfig_parser_get_tries_done (OstreeBootconfigParser *self)
{
  return self->tries_done;
}

/**
 * ostree_bootconfig_parser_get_tries_done:
 * @self: Parser
 *
 * Returns: TRUE if the boot config was parsed from existing boot config file
 */
gboolean
ostree_bootconfig_parser_is_parsed (OstreeBootconfigParser *self)
{
  return self->parsed;
}

/**
 * ostree_bootconfig_parser_parse_at:
 * @self: Parser
 * @dfd: Directory fd
 * @path: File path
 * @cancellable: Cancellable
 * @error: Error
 *
 * Initialize a bootconfig from the given file.
 */
gboolean
ostree_bootconfig_parser_parse_at (OstreeBootconfigParser *self, int dfd, const char *path,
                                   GCancellable *cancellable, GError **error)
{
  g_assert (!self->parsed);

  g_autofree char *contents = glnx_file_get_contents_utf8_at (dfd, path, NULL, cancellable, error);
  if (!contents)
    return FALSE;

  g_autoptr (GPtrArray) overlay_initrds = NULL;

  g_auto (GStrv) lines = g_strsplit (contents, "\n", -1);
  for (char **iter = lines; *iter; iter++)
    {
      const char *line = *iter;

      if (g_ascii_isalpha (*line))
        {
          char **items = NULL;
          items = g_strsplit_set (line, self->separators, 2);
          if (g_strv_length (items) == 2 && items[0][0] != '\0')
            {
              if (g_str_equal (items[0], "initrd")
                  && g_hash_table_contains (self->options, "initrd"))
                {
                  if (!overlay_initrds)
                    overlay_initrds = g_ptr_array_new_with_free_func (g_free);
                  g_ptr_array_add (overlay_initrds, items[1]);
                  g_free (items[0]);
                }
              else
                {
                  g_hash_table_insert (self->options, items[0], items[1]);
                }
              g_free (items); /* Free container; we stole the elements */
            }
          else
            {
              g_strfreev (items);
            }
        }
    }

  if (overlay_initrds)
    {
      g_ptr_array_add (overlay_initrds, NULL);
      self->overlay_initrds = (char **)g_ptr_array_free (g_steal_pointer (&overlay_initrds), FALSE);
    }

  parse_bootloader_tries (glnx_basename (path), &self->tries_left, &self->tries_done, error);

  self->parsed = TRUE;

  return TRUE;
}

gboolean
ostree_bootconfig_parser_parse (OstreeBootconfigParser *self, GFile *path,
                                GCancellable *cancellable, GError **error)
{
  return ostree_bootconfig_parser_parse_at (self, AT_FDCWD, gs_file_get_path_cached (path),
                                            cancellable, error);
}

/**
 * ostree_bootconfig_parser_set:
 * @self: Parser
 * @key: the key
 * @value: the key
 *
 * Set the @key/@value pair to the boot configuration dictionary.
 */
void
ostree_bootconfig_parser_set (OstreeBootconfigParser *self, const char *key, const char *value)
{
  g_hash_table_replace (self->options, g_strdup (key), g_strdup (value));
}

/**
 * ostree_bootconfig_parser_get:
 * @self: Parser
 * @key: the key name to retrieve
 *
 * Get the value corresponding to @key from the boot configuration dictionary.
 *
 * Returns: (nullable): The corresponding value, or %NULL if the key hasn't been
 * found.
 */
const char *
ostree_bootconfig_parser_get (OstreeBootconfigParser *self, const char *key)
{
  return g_hash_table_lookup (self->options, key);
}

/**
 * ostree_bootconfig_parser_set_overlay_initrds:
 * @self: Parser
 * @initrds: (array zero-terminated=1) (transfer none) (allow-none): Array of overlay
 *    initrds or %NULL to unset.
 *
 * These are rendered as additional `initrd` keys in the final bootloader configs. The
 * base initrd is part of the primary keys.
 *
 * Since: 2020.7
 */
void
ostree_bootconfig_parser_set_overlay_initrds (OstreeBootconfigParser *self, char **initrds)
{
  g_assert (g_hash_table_contains (self->options, "initrd"));
  g_strfreev (self->overlay_initrds);
  self->overlay_initrds = g_strdupv (initrds);
}

/**
 * ostree_bootconfig_parser_get_overlay_initrds:
 * @self: Parser
 *
 * Returns: (array zero-terminated=1) (transfer none) (nullable): Array of initrds or %NULL
 * if none are set.
 *
 * Since: 2020.7
 */
char **
ostree_bootconfig_parser_get_overlay_initrds (OstreeBootconfigParser *self)
{
  return self->overlay_initrds;
}

static void
write_key (OstreeBootconfigParser *self, GString *buf, const char *key, const char *value)
{
  g_string_append (buf, key);
  g_string_append_c (buf, self->separators[0]);
  g_string_append (buf, value);
  g_string_append_c (buf, '\n');
}

gboolean
ostree_bootconfig_parser_write_at (OstreeBootconfigParser *self, int dfd, const char *path,
                                   GCancellable *cancellable, GError **error)
{
  /* Write the fields in a deterministic order, following what is used
   * in the bootconfig example of the BootLoaderspec document:
   * https://systemd.io/BOOT_LOADER_SPECIFICATION
   */
  const char *fields[] = { "title", "version", "options", "devicetree", "linux", "initrd" };
  g_autoptr (GHashTable) keys_written = g_hash_table_new (g_str_hash, g_str_equal);
  g_autoptr (GString) buf = g_string_new ("");

  for (guint i = 0; i < G_N_ELEMENTS (fields); i++)
    {
      const char *key = fields[i];
      const char *value = g_hash_table_lookup (self->options, key);
      if (value != NULL)
        {
          write_key (self, buf, key, value);
          g_hash_table_add (keys_written, (gpointer)key);
        }
    }

  /* Write overlay initrds */
  if (self->overlay_initrds && (g_strv_length (self->overlay_initrds) > 0))
    {
      /* we should've written the primary initrd already */
      g_assert (g_hash_table_contains (keys_written, "initrd"));
      for (char **it = self->overlay_initrds; it && *it; it++)
        write_key (self, buf, "initrd", *it);
    }

  /* Write unknown fields */
  GLNX_HASH_TABLE_FOREACH_KV (self->options, const char *, k, const char *, v)
    {
      if (g_hash_table_lookup (keys_written, k))
        continue;
      write_key (self, buf, k, v);
    }

  if (!glnx_file_replace_contents_at (dfd, path, (guint8 *)buf->str, buf->len,
                                      GLNX_FILE_REPLACE_NODATASYNC, cancellable, error))
    return FALSE;

  return TRUE;
}

gboolean
ostree_bootconfig_parser_write (OstreeBootconfigParser *self, GFile *output,
                                GCancellable *cancellable, GError **error)
{
  return ostree_bootconfig_parser_write_at (self, AT_FDCWD, gs_file_get_path_cached (output),
                                            cancellable, error);
}

static void
ostree_bootconfig_parser_finalize (GObject *object)
{
  OstreeBootconfigParser *self = OSTREE_BOOTCONFIG_PARSER (object);

  g_strfreev (self->overlay_initrds);
  g_hash_table_unref (self->options);

  G_OBJECT_CLASS (ostree_bootconfig_parser_parent_class)->finalize (object);
}

static void
ostree_bootconfig_parser_init (OstreeBootconfigParser *self)
{
  self->options = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
}

void
ostree_bootconfig_parser_class_init (OstreeBootconfigParserClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->finalize = ostree_bootconfig_parser_finalize;
}

OstreeBootconfigParser *
ostree_bootconfig_parser_new (void)
{
  OstreeBootconfigParser *self = NULL;

  self = g_object_new (OSTREE_TYPE_BOOTCONFIG_PARSER, NULL);
  self->separators = " \t";
  return self;
}
