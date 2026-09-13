--- Utility module that wraps ninja_syntax with some helpful functions related
--- to command line parsing and error handling.

-- Cache library functions.
local type, pairs, ipairs = type, pairs, ipairs
local pcall, error, assert = pcall, error, assert
local _s = string
local strfind = _s.find
local sub, match, gmatch, gsub = _s.sub, _s.match, _s.gmatch, _s.gsub
local format, rep, upper = _s.format, _s.rep, _s.upper
local _t = table
local insert, remove, concat, sort = _t.insert, _t.remove, _t.concat, _t.sort
local exit = os.exit
local io = io
local stdin, stdout, stderr = io.stdin, io.stdout, io.stderr

-- Global state for current file.
local g_fname, g_curline, g_indent, g_lineno, g_synclineno, g_arch
local g_errcount = 0

local wfatal

-- Emit an error. Processing continues with next statement.
local function werror(msg)
	error(format("%s:%s: error: %s:\n%s", g_fname, g_lineno, msg, g_curline), 0)
end

-- Emit a fatal error. Processing stops.
wfatal = function(msg)
	g_errcount = "fatal"
	werror(msg)
end

-- Print a warning. Processing continues.
local function wwarn(msg)
	stderr:write(format("%s:%s: warning: %s:\n%s\n",
	             g_fname, g_lineno, msg, g_curline))
end

-- Print caught error message. But suppress excessive errors.
local function wprinterr(...)
	if type(g_errcount) == "number" then
		-- Regular error.
		g_errcount = g_errcount + 1
		if g_errcount < 21 then -- Seems to be a reasonable limit.
			stderr:write(...)
		elseif g_errcount == 21 then
			stderr:write(g_fname, ":*: warning: too many errors (suppressed further messages).\n")
		end
	else
		-- Fatal error.
		stderr:write(...)
		return true -- Stop processing.
	end
end


-- Map holding all option handlers.
local opt_current

-- Print error and exit with error status.
local function opterror(...)
	stderr:write("configure.lua: ERROR: ", ...)
	stderr:write("\n")
	exit(1)
end

-- Get option parameter.
local function optparam(args)
	local argn = args.argn
	local p = args[argn]
	if not p then
		opterror("missing parameter for option `", opt_current, "'.")
	end
	args.argn = argn + 1
	return p
end

-- Parse single option. `opts` is the destination table option handlers
-- store parsed values into; it is threaded through to each handler as its
-- first argument so handlers never need to reach for module-level state.
local function parseopt(opt, opts, args, opt_map, opt_alias)
	opt_current = #opt == 1 and "-"..opt or "--"..opt
	local f = opt_map[opt] or opt_map[opt_alias[opt]]
	if not f then
		opterror("unrecognized option `", opt_current, "'. Try `--help'.\n")
	end
	f(opts, args)
end

-- Parse arguments.
-- `opts` may be pre-populated with defaults; parsed options are merged into
-- it (mutated in place) and it is also returned for convenience. Any single
-- remaining positional argument is stored as `opts.src_dir`.
local function parseargs(args, opts, opt_map, opt_alias)
	opts = opts or {}

	-- Process all option arguments.
	args.argn = 1
	repeat
		local a = args[args.argn]
		if not a then break end
		local lopt, opt = match(a, "^%-(%-?)(.+)")
		if not opt then break end
		args.argn = args.argn + 1
		if lopt == "" then
			-- Loop through short options.
			for o in gmatch(opt, ".") do parseopt(o, opts, args, opt_map, opt_alias) end
		else
			-- Long option.
			parseopt(opt, opts, args, opt_map, opt_alias)
		end
	until false

	-- Check for proper number of arguments. The positional LUAJIT_SRC_DIR
	-- argument is optional -- callers are expected to seed opts.src_dir
	-- with a default before calling parseargs.
	local nargs = #args - args.argn + 1
	if nargs > 1 then
		opterror("too many positional arguments. Try `--help'.\n")
	elseif nargs == 1 then
		opts.src_dir = args[args.argn]
	end

	return opts
end

------------------------------------------------------------------------------
local ninja = require("ninja_syntax")

return {
	wfatal=wfatal,
	werror=werror,
	wwarn=wwarn,
	wprinterr=wprinterr,
	opterror=opterror,
	optparam=optparam,
	parseopt=parseopt,
	parseargs=parseargs,

	escape=ninja.escape,
	expand=ninja.expand,
	as_list=ninja.as_list,
	escape_path=ninja.escape_path,
	Writer=ninja.Writer,
}


