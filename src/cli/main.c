/* The CLI is the engine's own dpcmd.c (compiled into libsf100core with main renamed to dpcmd_main). */
int dpcmd_main(int argc, char* argv[]);

int main(int argc, char* argv[])
{
    return dpcmd_main(argc, argv);
}
