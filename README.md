# RoadCraft Automatic Cargo Load 1.1.2

A Windows mod that lets you find nearby loose cargo on a radar and place it over the truck or trailer you are using. It recognizes logs, recyclable scrap, finished materials, and sellable objects. The mod handles placement and release. Secure the load in the game after it lands.

## Build and install

This source targets the 64-bit Windows RoadCraft build `8.0.PATCH.772884`. Use a 64-bit Windows build of TinyCC with its Windows headers and libraries. From PowerShell, run `./build.ps1` if `tcc.exe` is on your PATH, or run `./build.ps1 -Compiler 'path/to/tcc.exe'`.

With RoadCraft closed, copy `RoadCraftAutoload.dll`, `RoadCraftAutoload.profiles`, `RoadCraftAutoload.ini`, and `version.dll` from `build/` to the game's `bin/pc` directory beside the executable. The mod uses `version.dll` as its loader. Back up an existing `version.dll` before installing so you can restore it when uninstalling. To uninstall, close the game and remove the files you installed.

## What's new

- Fixed truck detection when the game retains another camera with stale coordinates.
- Radar selection starts with the closest cargo. Refresh preserves the selected object if it is still in range.
- Added more sellable objects and a water-pipe scrap variant to the radar.

## Controls

Open or close the radar with **F9** or by holding **LB + RB** and tapping **Y**. Click a marker or use **Previous / Next** to select cargo. On a controller, use **D-pad Left / Right** to select cargo and **X** to refresh.

Click **Load** or press **A** to hold the selected object above the bed. The radar hides and returns camera control to the game.

| Action | Keyboard | Controller |
| --- | --- | --- |
| Move cargo | WASD | D-pad |
| Turn left / right | Q / E | LT / RT |
| Lower / raise | F / R | LB / RB |
| Drop and reopen radar | Enter | A |

**Esc** cancels placement and returns the cargo to its starting position. The radar can identify some objects made of multiple physics bodies that still require the crane for loading.

Demo - https://www.youtube.com/watch?v=CeRYsmqoq8Q
