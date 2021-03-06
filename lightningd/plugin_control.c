#include <ccan/opt/opt.h>
#include <lightningd/options.h>
#include <lightningd/plugin_control.h>
#include <lightningd/plugin_hook.h>
#include <sys/stat.h>
#include <sys/types.h>

/* A dummy structure used to give multiple arguments to callbacks. */
struct dynamic_plugin {
	struct plugin *plugin;
	struct command *cmd;
};

/**
 * Returned by all subcommands on success.
 */
static struct command_result *plugin_dynamic_list_plugins(struct command *cmd,
							  const struct plugins *plugins)
{
	struct json_stream *response;
	const struct plugin *p;

	response = json_stream_success(cmd);
	json_array_start(response, "plugins");
	list_for_each(&plugins->plugins, p, list) {
		json_object_start(response, NULL);
		json_add_string(response, "name", p->cmd);
		json_add_bool(response, "active",
		              p->plugin_state == INIT_COMPLETE);
		json_object_end(response);
	}
	json_array_end(response);
	return command_success(cmd, response);
}

struct command_result *plugin_cmd_killed(struct command *cmd,
					 struct plugin *plugin, const char *msg)
{
	return command_fail(cmd, PLUGIN_ERROR, "%s: %s", plugin->cmd, msg);
}

struct command_result *plugin_cmd_succeeded(struct command *cmd,
					    struct plugin *plugin)
{
	return plugin_dynamic_list_plugins(cmd, plugin->plugins);
}

struct command_result *plugin_cmd_all_complete(struct plugins *plugins,
					       struct command *cmd)
{
	return plugin_dynamic_list_plugins(cmd, plugins);
}

/**
 * Called when trying to start a plugin through RPC, it starts the plugin and
 * will give a result 60 seconds later at the most (once init completes).
 */
static struct command_result *
plugin_dynamic_start(struct command *cmd, const char *plugin_path)
{
	struct plugin *p = plugin_register(cmd->ld->plugins, plugin_path, cmd);
	const char *err;

	if (!p)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				    "%s: already registered",
				    plugin_path);

	/* This will come back via plugin_cmd_killed or plugin_cmd_succeeded */
	err = plugin_send_getmanifest(p);
	if (err)
		return command_fail(cmd, PLUGIN_ERROR,
				    "%s: %s",
				    plugin_path, err);

	return command_still_pending(cmd);
}

/**
 * Called when trying to start a plugin directory through RPC, it registers
 * all contained plugins recursively and then starts them.
 */
static struct command_result *
plugin_dynamic_startdir(struct command *cmd, const char *dir_path)
{
	const char *err;
	struct command_result *res;

	err = add_plugin_dir(cmd->ld->plugins, dir_path, false);
	if (err)
		return command_fail(cmd, JSONRPC2_INVALID_PARAMS, "%s", err);

	/* If none added, this calls plugin_cmd_all_complete immediately */
	res = plugin_register_all_complete(cmd->ld, cmd);
	if (res)
		return res;

	plugins_send_getmanifest(cmd->ld->plugins);
	return command_still_pending(cmd);
}

static struct command_result *
plugin_dynamic_stop(struct command *cmd, const char *plugin_name)
{
	struct plugin *p;
	struct json_stream *response;

	list_for_each(&cmd->ld->plugins->plugins, p, list) {
		if (plugin_paths_match(p->cmd, plugin_name)) {
			if (!p->dynamic)
				return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
				                    "%s cannot be managed when "
				                    "lightningd is up",
				                    plugin_name);
			plugin_kill(p, "stopped by lightningd via RPC");
			response = json_stream_success(cmd);
			if (deprecated_apis)
				json_add_string(response, "",
			                    take(tal_fmt(NULL, "Successfully stopped %s.",
			                                 plugin_name)));
			json_add_string(response, "result",
			                take(tal_fmt(NULL, "Successfully stopped %s.",
			                             plugin_name)));
			return command_success(cmd, response);
		}
	}

	return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
	                    "Could not find plugin %s", plugin_name);
}

/**
 * Look for additions in the default plugin directory.
 */
static struct command_result *
plugin_dynamic_rescan_plugins(struct command *cmd)
{
	struct command_result *res;

	/* This will not fail on "already registered" error. */
	plugins_add_default_dir(cmd->ld->plugins);

	/* If none added, this calls plugin_cmd_all_complete immediately */
	res = plugin_register_all_complete(cmd->ld, cmd);
	if (res)
		return res;

	return command_still_pending(cmd);
}

/**
 * A plugin command which permits to control plugins without restarting
 * lightningd. It takes a subcommand, and an optional subcommand parameter.
 */
static struct command_result *json_plugin_control(struct command *cmd,
						const char *buffer,
						const jsmntok_t *obj UNNEEDED,
						const jsmntok_t *params)
{
	const char *subcmd;
	subcmd = param_subcommand(cmd, buffer, params,
	                          "start", "stop", "startdir",
	                          "rescan", "list", NULL);
	if (!subcmd)
		return command_param_failed();

	if (streq(subcmd, "stop")) {
		const char *plugin_name;

		if (!param(cmd, buffer, params,
			   p_req("subcommand", param_ignore, cmd),
			   p_req("plugin", param_string, &plugin_name),
			   NULL))
			return command_param_failed();

		return plugin_dynamic_stop(cmd, plugin_name);
	} else if (streq(subcmd, "start")) {
		const char *plugin_path;

		if (!param(cmd, buffer, params,
			   p_req("subcommand", param_ignore, cmd),
			   p_req("plugin", param_string, &plugin_path),
			   NULL))
			return command_param_failed();

		if (access(plugin_path, X_OK) == 0)
			return plugin_dynamic_start(cmd, plugin_path);
		else
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						   "%s is not executable: %s",
						   plugin_path, strerror(errno));
	} else if (streq(subcmd, "startdir")) {
		const char *dir_path;

		if (!param(cmd, buffer, params,
			   p_req("subcommand", param_ignore, cmd),
			   p_req("directory", param_string, &dir_path),
			   NULL))
			return command_param_failed();

		if (access(dir_path, F_OK) == 0)
			return plugin_dynamic_startdir(cmd, dir_path);
		else
			return command_fail(cmd, JSONRPC2_INVALID_PARAMS,
						   "Could not open %s", dir_path);
	} else if (streq(subcmd, "rescan")) {
		if (!param(cmd, buffer, params,
			   p_req("subcommand", param_ignore, cmd),
			   NULL))
			return command_param_failed();

		return plugin_dynamic_rescan_plugins(cmd);
	} else if (streq(subcmd, "list")) {
		if (!param(cmd, buffer, params,
			   p_req("subcommand", param_ignore, cmd),
			   NULL))
			return command_param_failed();

		return plugin_dynamic_list_plugins(cmd, cmd->ld->plugins);
	}

	/* subcmd must be one of the above: param_subcommand checked it! */
	abort();
}

static const struct json_command plugin_control_command = {
	"plugin",
	"plugin",
	json_plugin_control,
	"Control plugins (start, stop, startdir, rescan, list)",
	.verbose = "Usage :\n"
	"plugin start /path/to/a/plugin\n"
	"	adds a new plugin to c-lightning\n"
	"plugin stop plugin_name\n"
	"	stops an already registered plugin\n"
	"plugin startdir /path/to/a/plugin_dir/\n"
	"	adds a new plugin directory\n"
	"plugin rescan\n"
	"	loads not-already-loaded plugins from the default plugins dir\n"
	"plugin list\n"
	"	lists all active plugins\n"
	"\n"
};
AUTODATA(json_command, &plugin_control_command);
