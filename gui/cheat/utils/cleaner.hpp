#pragma once

namespace Utils {
    class Cleaner {
    public:
        // Removes the vulnerable intel driver / cheat service registry keys and
        // the BAM/DAM execution traces. Requires administrator rights.
        static void SanitizeRegistryTraces();
    };
}
