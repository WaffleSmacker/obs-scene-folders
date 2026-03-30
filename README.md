# Scene Folders for OBS

Organize your OBS scenes into folders. If you have dozens of scenes, this plugin adds a new dock panel where you can group them into collapsible folders for quick access.

## Features

- **Create folders** to group related scenes together
- **Drag and drop** scenes into folders
- **Double-click** any scene to instantly switch to it
- **Right-click** for options: create, rename, delete folders; move scenes between folders
- **Persistent** - your folder organization is saved and restored across OBS sessions
- **Auto-syncs** - new scenes appear automatically, deleted scenes are removed
- **Current scene highlighting** - the active scene is always highlighted in the tree

## Installation

1. Download the latest `obs-scene-folders.dll` from the [Releases](https://github.com/WaffleSmacker/obs-scene-folders/releases) page.
2. Copy the DLL to your OBS plugins folder:
   - **Default install:** `C:\Program Files\obs-studio\obs-plugins\64bit\`
   - **Custom install:** `<your OBS path>\obs-plugins\64bit\`
3. Restart OBS.
4. The "Scene Folders" dock will appear automatically. You can also toggle it from **View > Docks > Scene Folders**.

## Usage

1. **Create a folder** - Right-click on empty space in the Scene Folders dock and select "New Folder".
2. **Move a scene into a folder** - Either drag and drop it, or right-click the scene and use "Move to Folder".
3. **Switch scenes** - Double-click any scene in the tree.
4. **Rename a folder** - Right-click the folder and select "Rename Folder".
5. **Delete a folder** - Right-click the folder and select "Delete Folder". Scenes inside will be moved back to the top level.
6. **Remove a scene from a folder** - Right-click the scene and select "Remove from Folder".

## Building from Source

### Requirements

- Visual Studio 2022
- CMake 3.28+

### Build Steps

```bash
cmake --preset windows-x64
cmake --build --preset windows-x64
```

Dependencies (OBS SDK, Qt6) are downloaded automatically during the configure step.

## License

This project is licensed under the GNU General Public License v2.0 - see the [LICENSE](LICENSE) file for details.
