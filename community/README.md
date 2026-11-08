# Community apps

Apps shared by their authors as a built `.papp`, without the source. After a pull request adding one is
merged, the app appears in the **Store** under **Community**, with its author, and installs into the
Community folder on the home screen. Community apps are not reviewed: see
[Sharing your app](../README.md#sharing-your-app) in the main README before you install or submit one.

## Share an app

1. Build it with the SDK and give it a version in `version.txt` beside the source (`1.0.0`).
   Its id (in `PAPP_MANIFEST`) must be your own, such as `yourname.game`; ids starting with `emu.`,
   `game.`, `test.`, `sys.` or `lx.` belong to the project.
2. Make a folder named like the app, lowercase (`community/my-game/`), holding:

   | File | |
   |---|---|
   | `my-game.papp` | the app, at most 4 MB |
   | `icon.png` | square PNG, 64 to 256 pixels, 88×88 is best, transparency OK |
   | `app.json` | copy [`_template/app.json`](_template/app.json) and fill it in |
   | `screenshot.png` | optional: `python tools/papp_dev.py shot screenshot.png` while it runs |

   `app.json` holds `author`, a one-line `description`, the `license`, optionally a `category` (`games`,
   `tools`, `productivity`, `education` or `emulators`: the Store lists the app there too) and, if you publish it, a `source`
   link. An app bigger than 4 MB stays on your own release page: leave the `.papp` out and add
   `"url"` (https link to it), `"sha256"` and `"size"` to `app.json`.
3. Check it: `python tools/community.py`.
4. Open a pull request. Without git: fork the repository on GitHub, open `community/`, choose
   **Add file → Upload files**, drop your folder in and propose the change.

A check runs on the pull request and lists anything to fix. To update your app, bump its version and send
the new `.papp` the same way.

**Licences:** an app built on GPL code (most emulator cores, nofrendo) cannot be shared as a binary
alone: its source has to be published too. Put the link in `source`.
