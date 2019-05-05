#include <iostream>
#include <experimental/filesystem>

#include "tinymesh/tinymesh.h"

namespace fs = std::experimental::filesystem;

int main(int argc, char **argv) {
    if (argc <= 1) {
        std::cout << "usage: hello_tinymesh [input mesh] [ratio]" << std::endl;
        return 1;
    }

    const double ratio = argc <= 2 ? 0.1 : atof(argv[2]);

    // Load
    tinymesh::Mesh mesh(argv[1]);

    // Simplify
    const int target = (int)(ratio * mesh.num_vertices());
    tinymesh::simplifyIncremental(mesh, target);
    tinymesh::holeFill(mesh);
    tinymesh::remeshIncremental(mesh);

    // Save
    const fs::path filepath = fs::canonical(fs::path(argv[1]));
    const fs::path dirpath = filepath.parent_path();
    const std::string extension = filepath.extension().string();
    const std::string basename = filepath.stem().string();
    const std::string outfile = (dirpath / fs::path((basename + "_simplify" + extension).c_str())).string();
    mesh.save(outfile);
    printf("Save: %s\n", outfile.c_str());
}
