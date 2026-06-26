import std;
int main() {
    std::println("import std; works with CMake 4.3.2 + GCC 16!");
    std::vector<int> v = {1, 2, 3};
    std::println("vector: {}, {}, {}", v[0], v[1], v[2]);
    return 0;
}
