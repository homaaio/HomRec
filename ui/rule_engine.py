"""
rule_engine.py — HomRec 2.0.0 Rule Engine

Rules are stored in rules.json next to homrec_settings.json.
Each rule is evaluated when a trigger fires.

Rule structure:
    {
        "night_mode": {
            "active": true,
            "trigger": "on_minute_change",
            "condition": "int(env['HOUR']) >= 20",
            "action": "!theme --set dark"
        }
    }

Usage:
    engine = RuleEngine(dispatch_fn=my_console_dispatch)
    engine.trigger("on_minute_change", {"HOUR": 22, "MINUTE": 0})
    engine.create("my_rule", trigger="on_recording_start",
                  condition="True", action="!stat")
"""

import os
import json
import logging
import datetime

log = logging.getLogger("homrec.rule_engine")

RULES_FILE = "rules.json"

# Available triggers — engine fires these; plugins can fire custom ones
BUILTIN_TRIGGERS = {
    "on_recording_start":  "Запись началась",
    "on_recording_stop":   "Запись остановлена",
    "on_recording_pause":  "Запись на паузе",
    "on_recording_resume": "Запись возобновлена",
    "on_minute_change":    "Каждую минуту",
    "on_app_start":        "Запуск приложения",
    "on_app_close":        "Закрытие приложения",
    "on_theme_change":     "Смена темы",
    "on_plugin_load":      "Загрузка плагина",
    "on_plugin_unload":    "Выгрузка плагина",
}


class RuleEngine:
    def __init__(self, dispatch_fn=None, rules_path: str = RULES_FILE):
        self._dispatch = dispatch_fn   # function(cmd: str) — runs a console command
        self._path = rules_path
        self._rules: dict = {}
        self._load()
        log.info(f"RuleEngine initialized: {len(self._rules)} rule(s) loaded")

    # -- Persistence ------------------------------------------------------------

    def _load(self) -> None:
        if os.path.exists(self._path):
            try:
                with open(self._path, "r", encoding="utf-8") as f:
                    self._rules = json.load(f)
                log.debug(f"Rules loaded from {self._path}")
            except Exception as e:
                log.warning(f"Could not load rules: {e}")
                self._rules = {}

    def _save(self) -> None:
        try:
            with open(self._path, "w", encoding="utf-8") as f:
                json.dump(self._rules, f, indent=2, ensure_ascii=False)
        except Exception as e:
            log.warning(f"Could not save rules: {e}")

    # -- CRUD -------------------------------------------------------------------

    def create(self, name: str, trigger: str, condition: str,
               action: str, active: bool = True) -> bool:
        """Create or overwrite a rule. Returns False if name is invalid."""
        name = name.strip()
        if not name:
            log.warning("Rule name cannot be empty")
            return False
        self._rules[name] = {
            "active":    active,
            "trigger":   trigger.strip(),
            "condition": condition.strip(),
            "action":    action.strip(),
        }
        self._save()
        log.info(f"Rule created: '{name}'")
        return True

    def delete(self, name: str) -> bool:
        if name in self._rules:
            del self._rules[name]
            self._save()
            log.info(f"Rule deleted: '{name}'")
            return True
        return False

    def edit(self, name: str, **kwargs) -> bool:
        """Edit specific fields of an existing rule."""
        if name not in self._rules:
            log.warning(f"Rule not found: '{name}'")
            return False
        for k, v in kwargs.items():
            if k in ("active", "trigger", "condition", "action"):
                self._rules[name][k] = v
        self._save()
        log.info(f"Rule edited: '{name}' → {kwargs}")
        return True

    def enable(self, name: str) -> bool:
        return self.edit(name, active=True)

    def disable(self, name: str) -> bool:
        return self.edit(name, active=False)

    def list_rules(self) -> dict:
        return dict(self._rules)

    def get(self, name: str) -> dict | None:
        return self._rules.get(name)

    # -- Engine -----------------------------------------------------------------

    def trigger(self, trigger_name: str, env: dict | None = None) -> int:
        """
        Fire a trigger. Evaluates conditions of matching active rules
        and dispatches their actions.
        Returns number of rules that fired.
        """
        if env is None:
            now = datetime.datetime.now()
            env = {
                "HOUR":   now.hour,
                "MINUTE": now.minute,
                "SECOND": now.second,
                "WEEKDAY": now.weekday(),   # 0=Mon … 6=Sun
                "DAY":    now.day,
                "MONTH":  now.month,
            }

        fired = 0
        for rule_name, rule in list(self._rules.items()):
            if not rule.get("active"):
                continue
            if rule.get("trigger") != trigger_name:
                continue
            try:
                result = eval(  # noqa: S307
                    rule["condition"],
                    {"__builtins__": {"int": int, "float": float, "str": str,
                                      "bool": bool, "abs": abs, "len": len,
                                      "min": min, "max": max, "round": round}},
                    {"env": env}
                )
            except Exception as e:
                log.warning(f"Rule '{rule_name}' condition error: {e}")
                continue

            if result:
                log.info(f"Rule '{rule_name}' fired → {rule['action']}")
                if self._dispatch:
                    try:
                        self._dispatch(rule["action"])
                    except Exception as e:
                        log.warning(f"Rule '{rule_name}' action error: {e}")
                fired += 1

        return fired

    # -- Pretty list (for console output) --------------------------------------

    def format_list(self) -> str:
        if not self._rules:
            return "  No rules defined. Use !create --rule to add one."
        col_n = max(len(n) for n in self._rules) + 2
        col_t = 20
        col_a = 28
        sep   = f"+{'-'*(col_n+2)}+{'-'*9}+{'-'*(col_t+2)}+{'-'*(col_a+2)}+"
        hdr   = (f"| {'Name':<{col_n}} | {'Status':<7} "
                 f"| {'Trigger':<{col_t}} | {'Action':<{col_a}} |")
        rows  = [sep, hdr, sep]
        for name, rule in self._rules.items():
            status = "ENABLED" if rule.get("active") else "DISABLED"
            rows.append(
                f"| {name:<{col_n}} | {status:<7} "
                f"| {rule.get('trigger',''):<{col_t}} "
                f"| {rule.get('action',''):<{col_a}} |"
            )
        rows.append(sep)
        return "\n".join(rows)
