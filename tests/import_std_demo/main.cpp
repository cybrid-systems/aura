import std;
int main() {
    std::cout << "import std; works with CMake 4.3.2 + GCC 16!" << std::endl;
    std::vector<int> v = {1, 2, 3};
    std::println("vector: {}, {}, {}", v[0], v[1], v[2]);
    return 0;
}
