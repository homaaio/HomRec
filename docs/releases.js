/* HomRec docs - live sync with GitHub Releases.
 *
 * What it does:
 *   - down-win.html : fills <tbody id="releases-body"> with one row per release asset
 *   - every page    : fills every [data-latest-version] element with the latest tag (e.g. "v2.4.2")
 *   - index.html    : [data-latest-installer] gets the href of the latest .exe installer
 *
 * Nothing to edit after a release: publish the release on GitHub and the site follows.
 * Results are cached in localStorage for 10 minutes so the unauthenticated GitHub API
 * limit (60 requests/hour per IP) is never a problem.
 *
 * Extra (non-GitHub) mirrors can't be discovered automatically; add them to MIRRORS below.
 */
(function () {
    "use strict";

    var REPO = "homaaio/HomRec";
    var API_URL = "https://api.github.com/repos/" + REPO + "/releases?per_page=100";
    var RELEASES_PAGE = "https://github.com/" + REPO + "/releases";
    var CACHE_KEY = "homrec_releases_v1";
    var CACHE_TTL_MS = 10 * 60 * 1000;

    // Manual mirrors: { "<tag>": { "<asset file name>": [ ["label", "https://..."] ] } }
    var MIRRORS = {
        "v2.0": {
            "HomRec-2.0-win64.zip": [
                ["google-drive", "https://drive.google.com/file/d/1SwhsZrVgYfBkjeZY4fM4P4B5eizT3Di9/view?usp=sharing"]
            ],
            "HomRec-2.0-win64.tar.gz": [
                ["google-drive", "https://drive.google.com/file/d/15YLW-gSxpmCHu7zW2wWLN7OLuHQRcPiD/view?usp=sharing"]
            ]
        }
    };

    function cacheRead(allowStale) {
        try {
            var raw = window.localStorage.getItem(CACHE_KEY);
            if (!raw) return null;
            var obj = JSON.parse(raw);
            if (!obj || !Array.isArray(obj.data)) return null;
            if (!allowStale && Date.now() - obj.time > CACHE_TTL_MS) return null;
            return obj.data;
        } catch (e) {
            return null;
        }
    }

    function cacheWrite(data) {
        try {
            window.localStorage.setItem(CACHE_KEY, JSON.stringify({ time: Date.now(), data: data }));
        } catch (e) { /* storage disabled - fine */ }
    }

    function loadReleases() {
        var fresh = cacheRead(false);
        if (fresh) return Promise.resolve(fresh);

        return fetch(API_URL, { headers: { "Accept": "application/vnd.github+json" } })
            .then(function (r) {
                if (!r.ok) throw new Error("GitHub API returned " + r.status);
                return r.json();
            })
            .then(function (list) {
                if (!Array.isArray(list)) throw new Error("Unexpected API response");
                // Keep only what we render, so the cache stays small.
                var slim = list.filter(function (rel) { return !rel.draft; }).map(function (rel) {
                    return {
                        tag: String(rel.tag_name || ""),
                        name: String(rel.name || rel.tag_name || ""),
                        url: String(rel.html_url || ""),
                        date: String(rel.published_at || rel.created_at || ""),
                        prerelease: !!rel.prerelease,
                        assets: (rel.assets || []).map(function (a) {
                            return {
                                name: String(a.name || ""),
                                size: Number(a.size) || 0,
                                url: String(a.browser_download_url || "")
                            };
                        })
                    };
                });
                cacheWrite(slim);
                return slim;
            })
            .catch(function (err) {
                var stale = cacheRead(true); // better old data than none
                if (stale) return stale;
                throw err;
            });
    }

    // Only ever link to github.com / known https hosts - never trust API strings blindly.
    function safeUrl(u) {
        return /^https:\/\/(github\.com|drive\.google\.com)\//.test(u) ? u : "";
    }

    function versionOf(tag) {
        return tag.replace(/^v/i, "");
    }

    function fmtSize(bytes) {
        if (!bytes) return "-";
        var mb = bytes / (1024 * 1024);
        if (mb >= 1) return (mb >= 100 ? mb.toFixed(0) : mb.toFixed(1)) + " MB";
        return Math.max(1, Math.round(bytes / 1024)) + " KB";
    }

    function fmtDate(iso) {
        return iso ? iso.slice(0, 10) : "-";
    }

    function isInstaller(name) {
        return /\.exe$/i.test(name);
    }

    function el(tag, text, cls) {
        var node = document.createElement(tag);
        if (text != null) node.textContent = text;
        if (cls) node.className = cls;
        return node;
    }

    function link(label, href) {
        var a = el("a", label);
        a.href = href;
        a.rel = "noopener";
        return a;
    }

    function appendLinks(cell, items) {
        items.forEach(function (it, i) {
            if (i) cell.appendChild(document.createTextNode(" "));
            cell.appendChild(link(it[0], it[1]));
        });
    }

    function row(rel, asset, showVersion) {
        var tr = document.createElement("tr");
        var ver = versionOf(rel.tag) + (rel.prerelease ? " (pre-release)" : "");
        tr.appendChild(el("td", showVersion ? ver : ""));
        tr.appendChild(el("td", showVersion ? fmtDate(rel.date) : ""));

        var linksCell = el("td");
        if (asset) {
            tr.appendChild(el("td", asset.name));
            tr.appendChild(el("td", fmtSize(asset.size)));
            var links = [];
            var dl = safeUrl(asset.url);
            if (dl) links.push(["[github]", dl]);
            var extra = (MIRRORS[rel.tag] || {})[asset.name] || [];
            extra.forEach(function (m) { links.push(["[" + m[0] + "]", m[1]]); });
            appendLinks(linksCell, links);
        } else {
            tr.appendChild(el("td", "-"));
            tr.appendChild(el("td", "-"));
            var page = safeUrl(rel.url);
            if (page) appendLinks(linksCell, [["[release page - no files attached yet]", page]]);
        }
        tr.appendChild(linksCell);
        return tr;
    }

    function renderTable(releases) {
        var body = document.getElementById("releases-body");
        if (!body) return;
        body.textContent = "";
        if (!releases.length) {
            var empty = document.createElement("tr");
            var td = el("td", "No releases published yet.");
            td.colSpan = 5;
            empty.appendChild(td);
            body.appendChild(empty);
            return;
        }
        releases.forEach(function (rel) {
            var assets = rel.assets.filter(function (a) { return safeUrl(a.url); });
            if (!assets.length) {
                body.appendChild(row(rel, null, true));
                return;
            }
            assets.forEach(function (a, i) {
                body.appendChild(row(rel, a, i === 0));
            });
        });
    }

    function renderError() {
        var body = document.getElementById("releases-body");
        if (!body) return;
        body.textContent = "";
        var tr = document.createElement("tr");
        var td = el("td", "Could not load the release list from GitHub. ");
        td.colSpan = 5;
        td.appendChild(link("Open the releases page directly", RELEASES_PAGE));
        td.appendChild(document.createTextNode("."));
        tr.appendChild(td);
        body.appendChild(tr);
    }

    function renderLatest(releases) {
        var stable = releases.filter(function (r) { return !r.prerelease; });
        var latest = stable[0] || releases[0];
        if (!latest) return;

        var badges = document.querySelectorAll("[data-latest-version]");
        for (var i = 0; i < badges.length; i++) {
            badges[i].textContent = "v" + versionOf(latest.tag);
        }

        var installer = null;
        for (var j = 0; j < latest.assets.length; j++) {
            if (isInstaller(latest.assets[j].name) && safeUrl(latest.assets[j].url)) {
                installer = latest.assets[j];
                break;
            }
        }
        var targets = document.querySelectorAll("[data-latest-installer]");
        for (var k = 0; k < targets.length; k++) {
            if (installer) {
                targets[k].href = installer.url;
                targets[k].title = installer.name + " (" + fmtSize(installer.size) + ")";
            }
        }
    }

    function init() {
        loadReleases().then(function (releases) {
            renderLatest(releases);
            renderTable(releases);
        }).catch(function () {
            renderError();
        });
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", init);
    } else {
        init();
    }
})();
