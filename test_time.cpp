#include <iostream>
#include <ctime>
#include <iomanip>
#include <sstream>

int main() {
    std::string t_str = "2025-01-01 12:00:00";
    std::tm t = {};
    std::stringstream ss(t_str);
    ss >> std::get_time(&t, "%Y-%m-%d %H:%M:%S");

    // Test mktime (Local)
    std::tm t1 = t;
    time_t local = std::mktime(&t1);

    // Test timegm (UTC)
    std::tm t2 = t;
    t2.tm_isdst = 0;
    time_t utc = timegm(&t2);

    std::cout << "Input: " << t_str << std::endl;
    std::cout << "Local (mktime): " << local << std::endl;
    std::cout << "UTC (timegm):   " << utc << std::endl;
    std::cout << "Difference: " << (utc - local) / 3600.0 << " hours" << std::endl;

    return 0;
}
