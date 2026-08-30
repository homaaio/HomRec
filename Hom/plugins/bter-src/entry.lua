-- bter/entry.lua
--
-- "i know what i'll do" - a grab-bag utility plugin whose real job is
-- being something for `hom install bter` to actually download and load,
-- so the plugin package manager (see tools/hom/) has a live, harmless
-- target to test against instead of only the input-overlay presets
-- (which ship bundled in plugins/, so hom never actually has to fetch
-- them). See commands.md section 19 for the command list this file
-- implements.
--
-- Every command is registered through the single reg(name, description,
-- fn) helper below instead of 27 separate homrec.register_command()
-- calls, so COMMANDS (used by the "bter" command itself to print its own
-- list) and the actual registration can't drift apart, and adding one
-- more command later is a one-line addition near the bottom of this
-- file instead of a new call site.
--
-- Full filesystem/network access (io/os aren't sandboxed - see
-- lua_engine.h) is what lets whoami/hostname/ipconfig/etc below just
-- shell out to the real Windows tools of the same name via io.popen,
-- and homrec.http_get gives bping/myip real network access Lua's stdlib
-- doesn't have on its own.

local COMMANDS = {} -- filled in by reg() below; read by cmd_bter()

-- --- small string/shell helpers ----------------------------------------

local function trim(s)
    return (s:gsub("^%s+", ""):gsub("%s+$", ""))
end

-- Splits a raw console line into (first word, rest) the same way
-- ConsoleWindow::RunCommand() does: first whitespace-delimited token is
-- the command word, everything after it (trimmed) is the argument text.
local function split_first(raw)
    local name, rest = raw:match("^(%S*)%s*(.-)$")
    return name or "", rest or ""
end

-- Runs a shell command and returns its combined stdout+stderr as one
-- string, or nil if the command couldn't even be started. "2>&1" merges
-- stderr into the same pipe io.popen gives us - simplest way to not miss
-- an error message the tool wrote to stderr instead of stdout.
local function run_shell(cmd)
    local h = io.popen(cmd .. " 2>&1")
    if not h then return nil end
    local out = h:read("*a") or ""
    h:close()
    return out
end

-- Prints a shell command's output line-by-line, capped at `cap` lines
-- (nil = no cap) so a command like `tasks`/`netinfo` that can legitimately
-- return hundreds of lines doesn't flood the console - the full,
-- untruncated output still goes to logs\bter.log either way via
-- homrec.log_to(), so nothing is actually lost, just not all dumped into
-- the console at once.
local function print_shell(cmd, cap)
    local out = run_shell(cmd)
    if not out or trim(out) == "" then
        homrec.print(cmd .. ": no output (tool missing, or it failed)")
        return
    end
    homrec.log_to("bter.log", "[" .. cmd .. "]\n" .. out)
    local n = 0
    for line in out:gmatch("[^\r\n]+") do
        n = n + 1
        if cap and n > cap then
            homrec.print("  ... (" .. cap .. "+ lines - full output in logs\\bter.log)")
            return
        end
        homrec.print(line)
    end
end

-- --- base64 (hand-written - checked against the standard "Man"->"TWFu"
-- test vector, see cmd_b64encode/cmd_b64decode below) ---------------------

local B64_CHARS = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local B64_INDEX = {}
for i = 1, #B64_CHARS do B64_INDEX[B64_CHARS:sub(i, i)] = i - 1 end

local function b64_encode(data)
    local out = {}
    for i = 1, #data, 3 do
        local b1, b2, b3 = data:byte(i, i + 2)
        local chunk_len = math.min(3, #data - i + 1)
        b2 = b2 or 0
        b3 = b3 or 0
        local n = b1 * 65536 + b2 * 256 + b3
        local c1 = math.floor(n / 262144) % 64
        local c2 = math.floor(n / 4096) % 64
        local c3 = math.floor(n / 64) % 64
        local c4 = n % 64
        out[#out + 1] =
            B64_CHARS:sub(c1 + 1, c1 + 1) ..
            B64_CHARS:sub(c2 + 1, c2 + 1) ..
            (chunk_len >= 2 and B64_CHARS:sub(c3 + 1, c3 + 1) or "=") ..
            (chunk_len >= 3 and B64_CHARS:sub(c4 + 1, c4 + 1) or "=")
    end
    return table.concat(out)
end

local function b64_decode(data)
    data = data:gsub("[^%w%+/=]", "")
    local out = {}
    local i = 1
    while i + 1 <= #data do
        local c1 = B64_INDEX[data:sub(i, i)]
        local c2 = B64_INDEX[data:sub(i + 1, i + 1)]
        if not c1 or not c2 then break end
        local e3, e4 = data:sub(i + 2, i + 2), data:sub(i + 3, i + 3)
        local c3, c4 = B64_INDEX[e3], B64_INDEX[e4]
        local n = c1 * 262144 + c2 * 4096 + (c3 or 0) * 64 + (c4 or 0)
        out[#out + 1] = string.char(math.floor(n / 65536) % 256)
        if e3 ~= "=" and e3 ~= "" then out[#out + 1] = string.char(math.floor(n / 256) % 256) end
        if e4 ~= "=" and e4 ~= "" then out[#out + 1] = string.char(n % 256) end
        i = i + 4
    end
    return table.concat(out)
end

-- --- calc: hand-written recursive-descent parser for +,-,*,/,(,) only --
--
-- Deliberately NOT `load()`/`loadstring()` on the user's text, even
-- filtered - eval'ing arbitrary text as Lua would be a straight sandbox
-- escape given this plugin runtime has full io/os access (see the top of
-- this file), and a character filter alone isn't something worth trusting
-- to be airtight. This only ever does arithmetic, on numbers it parsed
-- itself - there's no way for `calc` to reach a function call or a
-- variable no matter what's typed.

local function calc_eval(expr)
    if expr:find("[^%d%.%+%-%*/%(%)%s]") then
        return nil, "only digits and + - * / ( ) are allowed"
    end

    local pos = 1
    local function peek() return expr:sub(pos, pos) end
    local function skip_ws() while peek() == " " do pos = pos + 1 end end

    local function parse_number()
        skip_ws()
        local s = pos
        if peek() == "-" then pos = pos + 1 end
        while peek():match("%d") do pos = pos + 1 end
        if peek() == "." then
            pos = pos + 1
            while peek():match("%d") do pos = pos + 1 end
        end
        if pos == s or (pos == s + 1 and expr:sub(s, s) == "-") then
            return nil, "expected a number at position " .. s
        end
        return tonumber(expr:sub(s, pos - 1))
    end

    local parse_expr

    local function parse_atom()
        skip_ws()
        if peek() == "(" then
            pos = pos + 1
            local v, err = parse_expr()
            if not v then return nil, err end
            skip_ws()
            if peek() ~= ")" then return nil, "missing ')'" end
            pos = pos + 1
            return v
        end
        return parse_number()
    end

    local function parse_term()
        local v, err = parse_atom()
        if not v then return nil, err end
        while true do
            skip_ws()
            local op = peek()
            if op ~= "*" and op ~= "/" then break end
            pos = pos + 1
            local rhs, e2 = parse_atom()
            if not rhs then return nil, e2 end
            if op == "*" then
                v = v * rhs
            else
                if rhs == 0 then return nil, "division by zero" end
                v = v / rhs
            end
        end
        return v
    end

    parse_expr = function()
        local v, err = parse_term()
        if not v then return nil, err end
        while true do
            skip_ws()
            local op = peek()
            if op ~= "+" and op ~= "-" then break end
            pos = pos + 1
            local rhs, e2 = parse_term()
            if not rhs then return nil, e2 end
            v = (op == "+") and (v + rhs) or (v - rhs)
        end
        return v
    end

    local result, err = parse_expr()
    if not result then return nil, err end
    skip_ws()
    if pos <= #expr then return nil, "unexpected text at position " .. pos end
    return result
end

-- --- inwid: bter's own settings/admin prefix ---------------------------
--
-- Stored in this plugin's own store (plugins/bter/.store via
-- homrec.store_get/store_set), NOT HomRec's core .hrc settings.
--
-- Honest limitation (see commands.md): all three `inwid settings` forms
-- below really do save their value, and `inwid status` really does read
-- it back - but none of them change real HomRec behaviour yet, because
-- HomRec doesn't expose an idle-detection hook, an updater-pause hook, or
-- an ffmpeg-argument hook to plugins. Each one says so every time it
-- runs, rather than quietly pretending the value does something.

local INWID_NOTE =
    "saved - but HomRec doesn't expose this hook to plugins yet, so it doesn't change real behaviour."

local function inwid_usage()
    homrec.print("usage:")
    homrec.print("  inwid status")
    homrec.print("  inwid settings afk-mode timeset <seconds> fpset <fps>")
    homrec.print("  inwid settings perver <true|false>")
    homrec.print("  inwid settings ffmpeg <arg string>")
end

local function inwid_status()
    homrec.print("inwid-managed settings (bter plugin):")
    homrec.print("  afk-mode.timeset = " .. homrec.store_get("inwid.afk_timeset", "(unset)"))
    homrec.print("  afk-mode.fpset   = " .. homrec.store_get("inwid.afk_fpset", "(unset)"))
    homrec.print("  perver           = " .. homrec.store_get("inwid.perver", "(unset)"))
    homrec.print("  ffmpeg           = " .. homrec.store_get("inwid.ffmpeg", "(unset)"))
end

local function cmd_inwid(raw)
    local _, rest = split_first(raw)
    if trim(rest) == "" then inwid_usage(); return end

    local sub, subrest = split_first(rest)
    if sub == "status" then
        inwid_status()
        return
    end
    if sub ~= "settings" then
        homrec.print("inwid: unknown subcommand '" .. sub .. "'")
        inwid_usage()
        return
    end

    local key, kv_rest = split_first(subrest)
    if key == "afk-mode" then
        local timeset = kv_rest:match("timeset%s+(%S+)")
        local fpset = kv_rest:match("fpset%s+(%S+)")
        if not timeset or not fpset or not tonumber(timeset) or not tonumber(fpset) then
            homrec.print("usage: inwid settings afk-mode timeset <seconds> fpset <fps>")
            return
        end
        homrec.store_set("inwid.afk_timeset", timeset)
        homrec.store_set("inwid.afk_fpset", fpset)
        homrec.print("afk-mode: timeset=" .. timeset .. " fpset=" .. fpset .. " -- " .. INWID_NOTE)
    elseif key == "perver" then
        local v = trim(kv_rest):lower()
        if v ~= "true" and v ~= "false" then
            homrec.print("usage: inwid settings perver <true|false>")
            return
        end
        homrec.store_set("inwid.perver", v)
        homrec.print("perver: " .. v .. " -- " .. INWID_NOTE)
    elseif key == "ffmpeg" then
        local v = trim(kv_rest)
        if v == "" then
            homrec.print("usage: inwid settings ffmpeg <arg string>")
            return
        end
        homrec.store_set("inwid.ffmpeg", v)
        homrec.print("ffmpeg args: \"" .. v .. "\" -- " .. INWID_NOTE)
    else
        homrec.print("inwid: unknown settings key '" .. key .. "'")
        inwid_usage()
    end
end

-- --- the rest of the command list ---------------------------------------

local function cmd_bter(raw)
    local info = homrec.plugin_info()
    local author = (info.author and info.author ~= "") and (" by " .. info.author) or ""

    -- "bter --version" / "bter -v": just the version line, nothing else -
    -- lets scripts/cfg files grep a single line instead of parsing the
    -- full about+command-list dump below.
    local _, rest = split_first(raw or "")
    rest = trim(rest)
    if rest == "--version" or rest == "-v" then
        homrec.print((info.name or "bter") .. " v" .. (info.version or "?") .. author)
        return
    end

    homrec.print((info.name or "bter") .. " v" .. (info.version or "?") .. author)
    homrec.print("\"i know what i'll do\" - a grab-bag utility plugin for testing hom / the console.")
    homrec.print("")
    homrec.print(#COMMANDS .. " commands:")
    for _, c in ipairs(COMMANDS) do
        homrec.print("  " .. c.name .. " - " .. c.desc)
    end
end

-- Same repo hom.exe itself reads Hom/ from (see tools/hom/hom.cpp's
-- k_raw_host/k_raw_path_prefix) - bping deliberately checks the same
-- endpoint hom's own commands depend on, not some unrelated URL.
local BPING_URL = "https://raw.githubusercontent.com/homaaio/HomRec/main/Hom/version.txt"

local function cmd_bping()
    -- os.clock() is a best-effort wall-clock stand-in here, not a real
    -- network RTT measurement the way hom.exe's own `hom ping` (C++,
    -- GetTickCount) is - Lua's stdlib has no high-resolution timer, and
    -- os.clock()'s exact semantics (CPU time vs. wall time) are
    -- implementation-defined. Good enough for "is it up and roughly how
    -- slow", not a benchmark.
    local t0 = os.clock()
    local body, err = homrec.http_get(BPING_URL)
    local dt_ms = math.floor((os.clock() - t0) * 1000)
    if not body or body == "" then
        homrec.print("bping: unreachable (" .. (err or "empty response") .. ")")
        return
    end
    homrec.print("bping: reachable, ~" .. dt_ms .. "ms (repo Hom/version.txt = \"" .. trim(body) .. "\")")
end

local function cmd_ffpath()
    local p = homrec.get_ffmpeg()
    if p and p ~= "" then
        homrec.print("ffmpeg: " .. p)
    else
        homrec.print("ffmpeg: not resolved yet")
    end
end

local function cmd_theme()
    local c = homrec.get_colors()
    for _, k in ipairs({ "bg", "fg", "accent", "success", "warning", "error", "surface", "text", "text_secondary" }) do
        homrec.print(string.format("  %-15s %s", k, c[k] or "?"))
    end
end

local function cmd_remember(raw)
    local _, rest = split_first(raw)
    local key, value = rest:match("^(%S+)%s+(.-)$")
    if not key or trim(value or "") == "" then
        homrec.print("usage: remember <key> <value>")
        return
    end
    homrec.store_set("note:" .. key, value)
    homrec.print("remembered '" .. key .. "'")
end

local function cmd_recall(raw)
    local _, rest = split_first(raw)
    local key = trim(rest)
    if key == "" then
        homrec.print("usage: recall <key>")
        return
    end
    local v = homrec.store_get("note:" .. key, "")
    if v == "" then
        homrec.print("recall: no note called '" .. key .. "'")
    else
        homrec.print(key .. " = " .. v)
    end
end

local function cmd_forget(raw)
    local _, rest = split_first(raw)
    local key = trim(rest)
    if key == "" then
        homrec.print("usage: forget <key>")
        return
    end
    -- homrec.store_set/store_get has no real delete (see lua_api.cpp) -
    -- this overwrites the note with an empty value, which recall/status
    -- already treat the same as "no note", instead of pretending there's
    -- a delete operation this API doesn't actually expose.
    homrec.store_set("note:" .. key, "")
    homrec.print("forgot '" .. key .. "'")
end

local function cmd_broadcast(raw)
    local _, msg = split_first(raw)
    if trim(msg) == "" then
        homrec.print("usage: broadcast <message>")
        return
    end
    homrec.emit("bter_broadcast", msg)
    homrec.print("broadcast: \"" .. msg .. "\" (event 'bter_broadcast', to every other loaded plugin's on_custom_event)")
end

local function cmd_toast(raw)
    local _, msg = split_first(raw)
    if trim(msg) == "" then
        homrec.print("usage: toast <message>")
        return
    end
    homrec.show_toast(msg)
    homrec.print("toast shown")
end

local function cmd_whoami() homrec.print(trim(run_shell("whoami") or "whoami: couldn't run whoami.exe")) end
local function cmd_hostname() homrec.print(trim(run_shell("hostname") or "hostname: couldn't run hostname.exe")) end
local function cmd_ipconfig() print_shell("ipconfig", nil) end
-- systeminfo.exe itself can take several seconds - that's the underlying
-- tool, not this wrapper; the command blocks the console until it
-- returns, same as every other subprocess-backed command here.
local function cmd_sysinfo() print_shell("systeminfo", nil) end
local function cmd_tasks() print_shell("tasklist", 40) end
local function cmd_netinfo() print_shell("netstat -an", 40) end
-- wmic is deprecated by Microsoft on newer Windows builds; swap this for
-- `Get-Volume` (PowerShell) if/when wmic actually stops being present.
local function cmd_diskfree() print_shell("wmic logicaldisk get Caption,FreeSpace,Size", nil) end

local function cmd_pinghost(raw)
    local _, host = split_first(raw)
    host = trim(host)
    if host == "" then
        homrec.print("usage: pinghost <host>")
        return
    end
    -- Restricted to letters/digits/./- only, same rule commands.md
    -- documents - rules out shell-metacharacter injection into the
    -- io.popen() call inside print_shell() (a bare host like
    -- "8.8.8.8; del *" would otherwise run as two commands).
    if host:find("[^%w%.%-]") then
        homrec.print("pinghost: host may only contain letters, digits, '.', '-' (got '" .. host .. "')")
        return
    end
    print_shell("ping -n 2 " .. host, nil)
end

local function cmd_myip()
    local body, err = homrec.http_get("https://api.ipify.org")
    if not body or trim(body) == "" then
        homrec.print("myip: couldn't reach the echo service (" .. (err or "empty response") .. ")")
        return
    end
    homrec.print("public IP: " .. trim(body))
end

local function cmd_clock() homrec.print(os.date("%Y-%m-%d %H:%M:%S")) end

local function cmd_calc(raw)
    local _, expr = split_first(raw)
    expr = trim(expr)
    if expr == "" then
        homrec.print("usage: calc <expression>  (+ - * / ( ) only, no functions/variables)")
        return
    end
    local result, err = calc_eval(expr)
    if not result then
        homrec.print("calc: " .. (err or "invalid expression"))
        return
    end
    homrec.print(expr .. " = " .. tostring(result))
end

local function cmd_coinflip() homrec.print(math.random(2) == 1 and "heads" or "tails") end

local function cmd_roll(raw)
    local _, spec = split_first(raw)
    spec = trim(spec)
    local n, m = spec:match("^(%d+)d(%d+)$")
    n, m = tonumber(n), tonumber(m)
    if not n or not m then
        homrec.print("usage: roll <NdM>, e.g. 'roll 2d6'")
        return
    end
    if n < 1 or n > 100 or m < 1 or m > 10000 then
        homrec.print("roll: N must be 1-100 and M must be 1-10000")
        return
    end
    local rolls, total = {}, 0
    for _ = 1, n do
        local r = math.random(m)
        rolls[#rolls + 1] = tostring(r)
        total = total + r
    end
    homrec.print(spec .. ": " .. table.concat(rolls, " + ") .. " = " .. total)
end

local function cmd_b64encode(raw)
    local _, text = split_first(raw)
    if text == "" then
        homrec.print("usage: b64encode <text>")
        return
    end
    homrec.print(b64_encode(text))
end

local function cmd_b64decode(raw)
    local _, text = split_first(raw)
    if trim(text) == "" then
        homrec.print("usage: b64decode <base64>")
        return
    end
    homrec.print(b64_decode(text))
end

local function cmd_revtext(raw)
    local _, text = split_first(raw)
    if text == "" then
        homrec.print("usage: revtext <text>")
        return
    end
    homrec.print(text:reverse())
end

local function cmd_wordcount(raw)
    local _, text = split_first(raw)
    if trim(text) == "" then
        homrec.print("usage: wordcount <text>")
        return
    end
    local n = 0
    for _ in text:gmatch("%S+") do n = n + 1 end
    homrec.print(n .. " word" .. (n == 1 and "" or "s") .. ", " .. #text .. " characters")
end

-- --- registration ---------------------------------------------------------

local function reg(name, desc, fn)
    COMMANDS[#COMMANDS + 1] = { name = name, desc = desc, fn = fn }
end

reg("bter", "About this plugin, lists every command below", cmd_bter)
reg("inwid", "bter's own settings/admin prefix (see commands.md)", cmd_inwid)
reg("bping", "HTTP reachability check against the plugin repo", cmd_bping)
reg("ffpath", "Prints the resolved ffmpeg path", cmd_ffpath)
reg("theme", "Prints the current theme's colours", cmd_theme)
reg("remember", "remember <key> <value> - saves a persistent note", cmd_remember)
reg("recall", "recall <key> - reads a note back", cmd_recall)
reg("forget", "forget <key> - clears a note", cmd_forget)
reg("broadcast", "broadcast <message> - emits a custom event other plugins can listen for", cmd_broadcast)
reg("toast", "toast <message> - shows a corner popup", cmd_toast)
reg("whoami", "Wraps whoami.exe", cmd_whoami)
reg("hostname", "Wraps hostname.exe", cmd_hostname)
reg("ipconfig", "Wraps ipconfig.exe", cmd_ipconfig)
reg("sysinfo", "Wraps systeminfo.exe", cmd_sysinfo)
reg("tasks", "Wraps tasklist.exe", cmd_tasks)
reg("netinfo", "Wraps netstat -an", cmd_netinfo)
reg("diskfree", "Wraps wmic logicaldisk (free/total space per drive)", cmd_diskfree)
reg("pinghost", "pinghost <host> - real ICMP ping via ping.exe", cmd_pinghost)
reg("myip", "Public IP via an external echo service", cmd_myip)
reg("clock", "Current local date/time", cmd_clock)
reg("calc", "calc <expression> - arithmetic only, + - * / ( )", cmd_calc)
reg("coinflip", "Flips a coin", cmd_coinflip)
reg("roll", "roll <NdM> - rolls N dice with M sides, e.g. 'roll 2d6'", cmd_roll)
reg("b64encode", "b64encode <text> - Base64 encode", cmd_b64encode)
reg("b64decode", "b64decode <base64> - Base64 decode", cmd_b64decode)
reg("revtext", "revtext <text> - reverses text", cmd_revtext)
reg("wordcount", "wordcount <text> - counts words/characters", cmd_wordcount)

function on_load()
    math.randomseed(os.time())
    for _, c in ipairs(COMMANDS) do
        homrec.register_command(c.name, c.desc, c.fn)
    end
    homrec.log("bter loaded: " .. #COMMANDS .. " commands registered.")
end
