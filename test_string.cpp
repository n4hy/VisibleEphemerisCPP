#include <iostream>
#include <string>
#include <algorithm>
#include <vector>

bool hasString(const std::string& haystack, const std::string& needle) {
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char ch1, char ch2) { return std::toupper(ch1) == std::toupper(ch2); }
    );
    return (it != haystack.end());
}

int main() {
    std::string s = "sun,moon";
    std::cout << "String: " << s << std::endl;
    std::cout << "Has sun? " << hasString(s, "sun") << std::endl;
    std::cout << "Has moon? " << hasString(s, "moon") << std::endl;
    return 0;
}
