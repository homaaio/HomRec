/* HomRec docs - instant search.
 * Splits the page into searchable blocks (every <h3> section, every
 * <details class="issue"> problem card, and each <h2> intro), then hides
 * blocks that don't contain ALL typed words. Matching problem cards open
 * automatically. Press "/" to focus the box, Esc to clear. Share a search
 * with a link like documentation.html#q=audio+sync
 */
(function () {
    "use strict";

    var box = document.getElementById("doc-search-input");
    var info = document.getElementById("doc-search-info");
    var content = document.querySelector(".content");
    if (!box || !content) return;

    var toc = content.querySelector(".toc");
    var sections = [];
    var groups = [];
    var section = null;
    var group = null;

    function isHeading(n, tag) { return n.nodeType === 1 && n.tagName === tag; }

    function startGroup(section, node) {
        group = { nodes: [], section: section, details: null };
        if (node) { group.nodes.push(node); }
        groups.push(group);
        section.groups.push(group);
        return group;
    }

    var start = document.getElementById("doc-search");
    var kids = Array.prototype.slice.call(content.children);
    var begun = false;

    kids.forEach(function (n) {
        if (n === start) { begun = true; return; }
        if (!begun || n.classList.contains("footer") || n === toc) return;

        if (isHeading(n, "H2")) {
            section = { heading: n, groups: [] };
            sections.push(section);
            startGroup(section, null);          // intro of the section
            return;
        }
        if (!section) return;                    // before the first h2
        if (isHeading(n, "H3") || (n.tagName === "DETAILS" && n.classList.contains("issue"))) {
            startGroup(section, n);
            if (n.tagName === "DETAILS") { group.details = n; group = null; }
            return;
        }
        if (!group) startGroup(section, null);
        group.nodes.push(n);
    });

    groups.forEach(function (g) {
        g.text = g.nodes.map(function (n) { return n.textContent; }).join(" ").toLowerCase();
        g.text += " " + g.section.heading.textContent.toLowerCase();
    });

    function setShown(nodes, shown) {
        nodes.forEach(function (n) { n.style.display = shown ? "" : "none"; });
    }

    function run() {
        var words = box.value.toLowerCase().split(/\s+/).filter(Boolean);
        var total = 0;

        sections.forEach(function (s) {
            var any = false;
            s.groups.forEach(function (g) {
                var ok = words.every(function (w) { return g.text.indexOf(w) !== -1; });
                var empty = !g.nodes.length;
                if (!words.length) ok = true;
                setShown(g.nodes, ok);
                if (g.details) { g.details.open = !!words.length && ok; }
                if (ok && !empty) { any = true; if (words.length) total++; }
            });
            s.heading.style.display = any ? "" : "none";
        });

        if (toc) toc.style.display = words.length ? "none" : "";
        if (!words.length) { info.textContent = ""; return; }
        info.textContent = total
            ? total + (total === 1 ? " match" : " matches") + " - Esc to clear"
            : "Nothing found. Try fewer or different words, or ask on Discord.";
    }

    var timer = null;
    box.addEventListener("input", function () {
        clearTimeout(timer);
        timer = setTimeout(run, 80);
    });

    document.addEventListener("keydown", function (e) {
        var typing = /^(INPUT|TEXTAREA|SELECT)$/.test((document.activeElement || {}).tagName || "");
        if (e.key === "/" && !typing) { e.preventDefault(); box.focus(); box.select(); }
        else if (e.key === "Escape" && document.activeElement === box) { box.value = ""; run(); box.blur(); }
    });

    var m = /[#&]q=([^&]*)/.exec(location.hash);
    if (m) {
        try { box.value = decodeURIComponent(m[1].replace(/\+/g, " ")); } catch (err) { /* ignore */ }
        run();
    }
})();
