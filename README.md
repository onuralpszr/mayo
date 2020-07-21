# Mayo
Mayo is a basic 3D viewer inspired by FreeCad.  

Current features are :
* Multi-documents support, user can open many parts in the session
* Support of STEP/IGES assemblies (colors and tree structure)
* Perspective/orthographic 3D view projection
* 3D clip planes with configurable capping
* 3D view cube providing intuitive camera manipulation
* Save image(snapshot) of the current 3D view
* Editable name of STEP/IGES entities
* Editable 3D properties of the imported items, eg. material, color, display mode, ...
* Area and volume properties for meshes and shapes

3D viewer operations :
* Rotate : mouse left + move
* Pan : mouse right + move
* Zoom : mouse wheel(scroll)
* Window zoom : mouse wheel + move
* Instant zoom : space bar

# Supported formats
  Formats                 |  Import   |  Export  | Notes
--------------------------|-----------|----------|------------------------------
STEP                      |  &#10004; | &#10004; | AP203, 214, 242(some parts)
IGES                      |  &#10004; | &#10004; | v5.3
OpenCascade BREP          |  &#10004; | &#10004; |
OBJ                       |  &#10004; | &#10007; | Requires OpenCascade &#8805; v7.4.0
STL                       |  &#10004; | &#10004; | ASCII/binary

# Build instructions
Mayo requires Qt5 and OpenCascade-7.3.0
Although only tested with VC++/Windows it should build fine on Linux and MacOS.  
It uses the `CSF_OCCTIncludePath` and `CSF_OCCTLibPath` environment variables to locate
OpenCascade include and lib paths. On Windows these two variables are set by the `env.bat`
script which can be found within OpenCascade's base folder. You should run this batch before
building mayo :  
`cd .../mayo`  
`qmake`  
`(n)make`  
In case you don't want to run this file you can use the `CASCADE_INC_DIR` and `CASCADE_LIB_DIR` qmake
variables instead :  
`qmake "CASCADE_INC_DIR=occ_include_dir" "CASCADE_LIB_DIR=occ_library_dir"`  

To enable optional gmio library, add this option to the qmake command line:  
`"GMIO_ROOT=path_to_gmio"`

# Screenshots

<img src="doc/screenshot_1.png"/>
