#include <iostream>
#include "../include/render/software/RenderTarget.hpp"

using namespace valo;

int main(int argc, char** argv) {
    if (argc <= 2) {
        std::cerr << "Incorrect usage. ./img-diff <path/to/image/1> <path/to/image/2>" << std::endl;
        return 1;
    }

    std::string path1 = argv[1];
    std::string path2 = argv[2];

    auto tgt1 = RenderTarget::loadImageFromPath(path1);
    auto tgt2 = RenderTarget::loadImageFromPath(path2);
    
    std::vector<uint32_t> img1Pixels(tgt1.data(), tgt1.data() + tgt1.width() * tgt1.height());
    std::vector<uint32_t> img2Pixels(tgt2.data(), tgt2.data() + tgt2.width() * tgt2.height());

    std::cout << "Image 1 Size: " << img1Pixels.size() << std::endl;
    std::cout << "Image 2 Size: " << img2Pixels.size() << std::endl;

    if (img1Pixels.size() != img2Pixels.size()) {
        std::cerr << "Image size mismatch: both images must be the same size." << std::endl;
        return 1;
    }

    float diffBits = 0;

    for (int i = 0; i < img1Pixels.size(); ++i) {
        auto px1 = img1Pixels[i];
        auto px2 = img2Pixels[i];

        auto x = px1 ^ px2;

        while (x) {
            diffBits += x & 1;
            x >>= 1;
        }
    }

    auto total = img1Pixels.size() * 32;
    auto pctDiff = 100.f * diffBits / total;
    std::cout << pctDiff << "% bit difference" << std::endl;

    return 0;
}