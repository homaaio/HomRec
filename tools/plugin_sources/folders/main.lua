-- Folders plugin for HomRec 2.0
-- Provides a UI entry point to browse .hrp packages and extract/install them.

function on_load()
    homrec.log.info("Folders plugin loaded")
    homrec.ui.add_menu_item("Settings", "Folders...", open_folders)
end

function on_help()
    return "Opens Settings → Folders... to browse .hrp packages in plugins/ and extract/install them."
end

function open_folders()
    -- Implemented by the host app (Python).
    if homrec.ui and homrec.ui.open_folders then
        homrec.ui.open_folders()
    else
        homrec.ui.show_dialog("Folders", "This HomRec build does not support open_folders().")
    end
end

