// Exercise a PE32 executable calling an export through its static IAT.
__declspec(dllimport) unsigned int Lib32Add(unsigned int a, unsigned int b);

unsigned int libcaller32_main(void) {
  return Lib32Add(19, 23);
}
