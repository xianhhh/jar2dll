extern "C" __declspec(dllimport) int j2d_chain_seed(int);
extern "C" __declspec(dllexport) int j2d_chain_step(int value) { return j2d_chain_seed(value) ^ 0x5A5A; }
