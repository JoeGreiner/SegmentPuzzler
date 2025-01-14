Tool to manipulate (merge, unmerge, regenerate, paint, select, ...) and proofread supervoxel-based segmentations. Under construction, more to come.

<video src="https://github.com/user-attachments/assets/44012456-4d42-4e00-93a5-8bacb68d5d3b" autoplay muted loop playsinline style="width:100%; max-width:800px; display:block; margin:0 auto;">
  Your browser does not support the video tag.
</video>


## Executables
| Windows | MacOSX | Linux |
|---------|---------|-------|
| <div align="center"><a href="https://github.com/JoeGreiner/SegmentPuzzler/releases"><img src="https://github.com/user-attachments/assets/78670603-900a-48f2-a857-6a0cc01cda93" width="100" alt="Windows"></a></div><br><a href="https://github.com/JoeGreiner/SegmentPuzzler/releases" style="text-decoration:none;color:inherit;">Installer, Portable</a> | <div align="center"><a href="https://github.com/JoeGreiner/SegmentPuzzler/releases"><img src="https://github.com/user-attachments/assets/975288b5-5e57-4e2e-9b41-17d38e590fbd" width="100" alt="MacOS"></a></div><br><a href="https://github.com/JoeGreiner/SegmentPuzzler/releases" style="text-decoration:none;color:inherit;">M1, Intel</a> | <div align="center"><a href="https://github.com/JoeGreiner/SegmentPuzzler/releases"><img src="https://github.com/user-attachments/assets/cb89df5c-52e2-4ca2-a388-f9c6ecdd0ef5" width="100" alt="Linux"></a></div><br><a href="https://github.com/JoeGreiner/SegmentPuzzler/releases" style="text-decoration:none;color:inherit;">AppImage (cross-distribution)</a> |

Please reach out if you encounter issues!

<details>
<summary>How to run an AppImage</summary>

AppImages should be compatible across different Linux distributions, as long as the libstdc++ isn't super old.     
* Download file, e.g. SegmentPuzzler.AppImage
* Terminal - set permissions to execute:
  ``` bash
  chmod u+x SegmentPuzzler.AppImage
  ```
* Terminal - execute AppImage:
  ``` bash
  ./SegmentPuzzler.AppImage
  ```
</details>

## Build from source

### Prerequisites

* CMake
* ITK 5 or 6 with ITKModuleReview enabled
* Qt 5
* Optional: OpenSSL (matching to the version Qt was built against). Only needed to download sample volumes.

<details>
<summary>Windows</summary>

1. **Clone the repository:**
    ```bash
    git clone https://github.com/JoeGreiner/SegmentPuzzler.git
    ```
2. **Open `CMakeLists.txt` with Qt Creator.**
3. **Configure the project.**
4. **Build the project.**
5. **Create a build directory and run CPack:**
    ```bash
    cd build
    cpack <build-dir>
    ```
    - This will generate both ZIP and NSIS installers.
6. **Run the installer to install SegmentPuzzler.**

</details>

<details>
<summary>Linux</summary>

1. **Clone the repository:**
    ```bash
    git clone https://github.com/JoeGreiner/SegmentPuzzler.git
    ```
2. **Create a build directory and navigate into it:**
    ```bash
    mkdir build && cd build
    ```
3. **Run CMake to configure the project:**
    ```bash
    cmake <source-dir>
    ```
4. **Build and install the project:**
    ```bash
    make install -j <number-of-cores>
    ```
5. **Deploy the application using the deployment script:**
    ```bash
    ./deploy/deploy_linux.sh
    ```
6. **Run the generated AppImage located in the build directory.**

</details>

<details>
<summary>macOS</summary>

1. **Clone the repository:**
    ```bash
    git clone https://github.com/JoeGreiner/SegmentPuzzler.git
    ```
2. **Create a build directory and navigate into it:**
    ```bash
    mkdir build && cd build
    ```
3. **Run CMake to configure the project:**
    ```bash
    cmake <source-dir>
    ```
4. **Build and install the project:**
    ```bash
    make install
    ```
5. **Package the application using CPack:**
    ```bash
    cpack -G DragNDrop <build-dir>
    ```
</details>

## Usage

Video tutorials and more documentation are (hopefully) coming soon!

<details>
<summary>Walk-Through</summary>

* Please click **`Sample Data -> Load Sample Data`** in the menu bar. You can load real data by simply dragging and dropping the volume files into the top-left tree viewer.
* You'll see that the viewer and the tree viewer in the top-left corner are populated with the sample data.
* **TreeViewer**: Here you can interact with the viewer options:
    * Clicking on the small box toggles the visibility of a segment.
    * Double-clicking on the segment name allows you to rename the segment.
    * Double-clicking on the **Color**, **Norm**, or **Alpha** values allows you to change them.
* You can find hotkeys in the **`Help`** menu.
* **Main Interactions**:
  SegmentPuzzler handles three kinds of partitions: **Initial Segments**, a **Working Set**, and **Final Segmentations**:
    * When you start segmenting, the **Working Set** is initialized with the initial segments, and the **Final Segmentations** are empty.
    * Merging or unmerging updates the **Working Set**, allowing you to return to the **Initial Segments**.
    * Once satisfied with the **Working Set**, transfer individual segments to the **Final Segmentations**, which can be saved and loaded.
* **Viewer Navigation**:
    * Scrolling with the mouse wheel moves through slices. You can also use the arrow keys (`↑` / `↓`) or the slider.
    * Pressing and holding the **Middle Mouse Button** while moving pans the view.
    * **`CTRL + Left Click`** centers all orthogonal views on the clicked point.
    * Pressing **`R`** recalculates LUTs (reshuffling the colors).
    * In an active viewer:
        * Press **`+`** to zoom in to the cursor.
        * Press **`-`** to zoom out.
* **Merging and Unmerging Segments**:
    * Paint with the **Left Mouse Button** to merge.
    * Paint with the **Right Mouse Button** to unmerge.
    * Change brush size with number keys **`0-9`**.
* **Reverting and Cutting Segments**:
    * Press **`X`** and click a segment to revert it to the initial segment.
    * Press **`C`** and click a segment to cut/isolate it.
* **Running Watershed (in `Probability` Tab)**:
    * Use the **`Run Watershed`** button to run a watershed algorithm on a probability map.
    * Run watershed only in a Region of Interest (ROI) by pressing the **`Turn ROI-Selection WS On`** button and drawing a rectangle in the viewer.
    * Note: Within the watershed dialog, you can paint boundaries and pre-merge the resulting watershed (recommended).
    * The resulting watershed is saved as a **Refinement** in the main viewer.
* **Refinements (in `Refinements` Tab)**:
    * Refinements are supervoxels that allow injecting smaller segments into your current segmentation.
    * Refinements can be loaded from a file or generated (e.g., from watershed).
    * Multiple refinements are supported. The currently active refinement is the one last clicked in the TreeViewer.
    * To inject segments:
        * Press **`P`**, then click a segment in the viewer to inject segments from the refinement.
        * This unmerges the clicked segment and injects the refinement's segments, updating the underlying graph.
* **Final Segmentations**:
    * **Transfer Segments**:
        * Press **`S`** and click on a segment in the viewer to transfer it to the **Final Segmentations**.
        * Delete segments from the **Final Segmentations** by pressing **`D`** and clicking the segment.
    * **Transfer by Volume**:
        * Use the buttons in the **`Segmentations`** tab to transfer by volume or transfer all segments.
    * **Painting in Final Segmentations**:
        * Activate Paintmode via the **`Turn on Paintmode`** button in the **`Segmentations`** tab.
        * Select a paint color by holding **`Q`** and clicking on a segment.
        * Paint with the **Left Mouse Button** to assign voxels to the selected segment.
        * Paint with the **Right Mouse Button** to remove voxels.
    * **Morphological Operations**:
        * Press **`F`** and click a segment to apply morphological closing (helpful for filling gaps).
        * Press **`G`** and click a segment to apply morphological opening (helpful for smoothing).
</details>


## Dependencies and Acknowledgments

A big thank you to the developers and contributors to:

* [ITK](https://github.com/InsightSoftwareConsortium/ITK): Used extensively for image handling and processing.
* [CMake](https://cmake.org/): Used to build the application.
* [Qt](https://www.qt.io/): Used for the GUI, licensed under the [LGPLv3](https://www.gnu.org/licenses/lgpl-3.0.en.html).
    - A copy of the license is included in the `licenses` directory.
    - The Qt framework is used unmodified in this project. Source code for Qt is available [here](https://download.qt.io/official_releases/).
    - Qt is dynamically linked in this application, allowing users to replace or update the Qt libraries. This can be done using the provided CMake build system, in compliance with LGPLv3 requirements.
* [qdarkstyle](https://github.com/ColinDuquesnoy/QDarkStyleSheet): Provides fantastic stylesheets for the application. Licensed under the MIT License, with a copy included in the `qdarkstyle` directory.
* [nitroshare](https://github.com/nitroshare/nitroshare-desktop): Inspiration for the `Windeployqt.cmake` script, which is licensed under the MIT License. A copy of the license can be found on top of the script.
* [linuxdeploy](https://github.com/linuxdeploy/linuxdeploy), [linuxdeploy-plugin-qt](https://github.com/linuxdeploy/linuxdeploy-plugin-qt), and [AppImageKit](https://github.com/AppImage/AppImageKit): Tools used to create a Linux AppImage.
* OS Icons are provided by [Iconoir](https://iconoir.com/), licensed under the MIT License.

## License

SegmentPuzzler is licensed under the [MIT License](LICENSE).
