#define NTDDI_VERSION NTDDI_WIN7
#define _WIN32_WINNT _WIN32_WINNT_WIN7

//#include <afxwin.h>
#include <windows.h>
#include <Psapi.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "gl3w/gl3w.c"

#define IMGUI_IMPL_OPENGL_LOADER_GL3W
#include "GL/gl3w.h"
#include "GLFW/glfw3.h"

#include "imgui/imgui.cpp"
#include "imgui/imgui_draw.cpp"
#include "imgui/imgui_widgets.cpp"
#include "imgui/imgui_demo.cpp"
#include "imgui/imgui_impl_glfw.cpp"
#include "imgui/imgui_impl_opengl3.cpp"

#define WRITABLE (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)

/* TODO:
 * - Sort pointer mapping output.
 * - Make address copyable with mouse.
 * - Improve pointer mapping.
 * - Improve pointer mapping performance.
 * - Figure out how to find stable addresses, meaning global addresses probably.
 * - Load all modules and specifically look for the "<process_name>.exe" one, instead of hoping it's the first one in the list.
 * - Fix all magic numbers.
 * - Check if there are any memory leaks.
 */

//
// DATA
//

// Array storing information about all sections of process memory that we care about (WRITABLE && COMMITTED)
MEMORY_BASIC_INFORMATION *ProcessData;

// Array storing data from all sections of process memory that we care about
char **MemoryBuffer;

// Total number of process memory sections that we care about
int NumRegions;

// "Attached" process handle
HANDLE ProcessHandle;
DWORD64 BaseAddress = 0;

// Struct storing information about a matching candidate
struct candidate_info {
    char *Address;
    void *PointerToCurValue; // Pointer to our copy
    int ValueSizeInBytes;
};

// Array storing all current candidates
#define MAX_CANDIDATES 2000000
candidate_info *Candidates = (candidate_info *) calloc(MAX_CANDIDATES, sizeof(*Candidates));
int *DeadCandidates = (int *) calloc(MAX_CANDIDATES, sizeof(*DeadCandidates));
int NumCandidates = -1;

// TODO: think abou this, is it too small? too big? (probably 4096 is safer?)
#define POINTER_MAX_RANGE 4096
struct pointer_node {
    char *Address;
    void *Value; // Pointer to our copy
    int NextJumpOffset; // Offset from *Pointer to the next pointer_node
    int NumJumps; // used to sort and filter results
};
// TODO: Not a full map, just a list of addresses with offsets to follow to get to the desired address, capped at 5 paths per origin.
//       We can add a "uncapped search in the future by being smart about merging paths and making sure we are not using up too many resources"
//       Need to think more about it.
// TODO: could use something else other than MAX_CANDIDATES here, not sure about what size of array we need.
pointer_node *PointerMap = (pointer_node *) calloc(MAX_CANDIDATES, sizeof(*PointerMap));
int NumPointerNodes = 0;

enum search_mode {
    MODE_NONE,
    MODE_BYTE,
    MODE_SHORT,
    MODE_INTEGER,
    MODE_POINTER,
    MODE_UNICODE,
    MODE_ASCII
};


//
// MAIN PROGRAM
//

static void GlfwErrorCallback(int error, const char* description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

#if 0
// UNUSED
void DumpMemoryRegionsInfo() {
    for (int I = 0; I < NumRegions; I++) {
        ImGui::Text("0x%p %lld\n", ProcessData[I].BaseAddress, ProcessData[I].RegionSize);
    }
}
#endif

// Scan the whole memory and builds an array of memory regions
void ScanEverything() {
    puts("-----------ScanEverything------------");
    char *Addr = 0;
    for (NumRegions = 0;;) {
        // read information about this region and store it in ProcessData[NumRegions]
        if (VirtualQueryEx(ProcessHandle, Addr, &ProcessData[NumRegions], sizeof(ProcessData[NumRegions])) == 0) {
            printf("Finished querying process memory.\nFound %d writable regions.\n", NumRegions);
            break;
        }

        // compute next address to query (before incrementing NumRegions)
        Addr = (char *) ProcessData[NumRegions].BaseAddress + ProcessData[NumRegions].RegionSize;

        // read all the memory from the current region and store it in MemoryBuffer[NumRegions]
        if ((ProcessData[NumRegions].State & MEM_COMMIT) &&
            !(ProcessData[NumRegions].Protect & PAGE_GUARD) &&
            (ProcessData[NumRegions].Protect & WRITABLE)) {
            if (MemoryBuffer[NumRegions]) {
                free(MemoryBuffer[NumRegions]);
            }
            MemoryBuffer[NumRegions] = (char *) calloc(ProcessData[NumRegions].RegionSize, sizeof(char));
            SIZE_T BytesRead;
            int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) ProcessData[NumRegions].BaseAddress, (LPVOID) MemoryBuffer[NumRegions], ProcessData[NumRegions].RegionSize, &BytesRead);
            if (!ReturnValue) {
                int ErrorValue = GetLastError();
                //fprintf(stderr, "ERROR: Tried to read from process handle 0x%p at address 0x%p and region size %lld with protection 0x%x\n", (LPVOID) ProcessHandle, (LPVOID) ProcessData[NumRegions].BaseAddress, ProcessData[NumRegions].RegionSize, ProcessData[NumRegions].Protect);
                //fprintf(stderr, "ERROR: BytesRead %lld\n", BytesRead);
                // TODO: does this ever happen? If we so, are we handling it correctly?
                //       Isn't it possible that not incrementing NumRegions here leads to problems? Maybe not. I think this is correct.
                fprintf(stderr, "ERROR: Problem occurred while reading region. Code: %d\n", ErrorValue); 
            } else {
                NumRegions++;
            }
        }
    }
    puts("-----------ScanEverything end------------");
}

// Reads every single candidate again (doesn't add new candidates)
void UpdateCandidates() {
    for (int I = 0; I < NumCandidates; I++) {
        SIZE_T BytesRead;

        // How to stop reading memory which has been freed? I assume we're getting some errors 299 for that reason.
        int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) Candidates[I].Address, (LPVOID) Candidates[I].PointerToCurValue, Candidates[I].ValueSizeInBytes, &BytesRead);
        if (!ReturnValue) {
            int ErrorValue = GetLastError();
            printf("ERROR: Problem occurred while reading region. Code: %d\n", ErrorValue);
        }
    }
}

// If there are no candidates, scans the whole memory and build a list of matching candidates
// Otherwise, scans the candidates list to filter it using a new value
void SearchForValue(char *Data, int Len, int Stride, bool IsPointer, bool ForceRescan) {
    fprintf(stderr, "SearchForValue(%p, %d, %d, %d) start\n", Data, Len, Stride, IsPointer);
    if (NumCandidates == -1) { // TODO: redo this, its a dirty hack for now
        NumCandidates = 0;
        
        if (ForceRescan)
            ScanEverything();

        for (int I = 0; I < NumRegions; I++) {
            for (int J = 0; J < ProcessData[I].RegionSize - Len; J+=Stride) {
                char *Candidate = (char *) &MemoryBuffer[I][J];
                //fprintf(stderr, "memcmp(0x%p, 0x%p, %d)\n", Candidate, Data, Len);

                if (NumCandidates >= MAX_CANDIDATES) { // TODO: protect against this overflow properly
                    fprintf(stderr, "ERROR: TOO MANY CANDIDATES!!\n");
                    abort();
                }

#if 0
                if (I % 1000 == 0 || I % 1000 == 1) {
                    fprintf(stderr, "AAAAAAAAA\n");
                    fprintf(stderr, "AAAAAAAAA testA 0x%llx\n", *((uint64_t*)Data));
                    fprintf(stderr, "AAAAAAAAA testB 0x%llx\n", *((uint64_t*)Candidate));
                }
#endif

                if (IsPointer) { // TODO: fix this, it's way too hack-ish, maybe use a separate SearchForPointer() function instead
                    if ((*((uint64_t*)Data) - *((uint64_t*)Candidate) > 0 &&
                        *((uint64_t*)Data) - *((uint64_t*)Candidate) < POINTER_MAX_RANGE)) {

#if 1
                        if (NumCandidates % 1000 == 0) {
                            fprintf(stderr, "NumCandidates for pointer with address 0x%llx == %d\n", *((uint64_t*)Data), NumCandidates);
                        }
#endif
                        Candidates[NumCandidates].Address = (char *) ProcessData[I].BaseAddress + J;
                        Candidates[NumCandidates].PointerToCurValue = (void *) &MemoryBuffer[I][J];
                        Candidates[NumCandidates].ValueSizeInBytes = Len;
                        NumCandidates++;
                    }
                } else if (!memcmp(Candidate, Data, Len)) {
#if 0
                    if (NumCandidates % 10000 == 0) {
                        fprintf(stderr, "NumCandidates == %d\n", NumCandidates);
                    }
#endif
#if 1
                    Candidates[NumCandidates].Address = (char *) ProcessData[I].BaseAddress + J;
                    Candidates[NumCandidates].PointerToCurValue = (void *) &MemoryBuffer[I][J];
                    Candidates[NumCandidates].ValueSizeInBytes = Len;
                    NumCandidates++;
#endif
                }
            }
        }
    } else {
        UpdateCandidates();

#if 1
        int NumDeadCandidates = 0;
        memset(DeadCandidates, 0, sizeof(*DeadCandidates));
        for (int I = 0; I < NumCandidates; I++) {
            if (IsPointer) {
                // TODO: fix this, it's way too hackish, maybe use a separate SearchForPointer() function instead
                if (!(*((uint64_t*)Data) - *((uint64_t*)Candidates[I].PointerToCurValue) > 0 &&
                    *((uint64_t*)Data) - *((uint64_t*)Candidates[I].PointerToCurValue) < POINTER_MAX_RANGE)) {
                    // mark candidate for deletion
                    DeadCandidates[NumDeadCandidates++] = I;
                }
            } else if (memcmp((char *)Candidates[I].PointerToCurValue, Data, Len)) {
                // mark candidate for deletion
                DeadCandidates[NumDeadCandidates++] = I;
            }
        }

        // delete dead candidates (could be optimized with memcpys probably, but it's weird)
        int AliveIndex = 0;
        int DeadIndex = 0;
        for (int I = 0; I < NumCandidates; I++) {
            if (DeadIndex < NumDeadCandidates && I == DeadCandidates[DeadIndex]) { // if Candidates[I] is marked as dead, don't copy it
                DeadIndex++;
            } else { // otherwise, copy it
                Candidates[AliveIndex++] = Candidates[I];
            }
        }
        NumCandidates = NumCandidates - NumDeadCandidates;
#endif
    }
    //fprintf(stderr, "SearchForValue() end\n");
}

// TODO: don't use the same NumCandidates Array? or make sure to clean up the Scan window?
void GeneratePointerMapRec(char *Address, int NumJumps, int MaxNumJumps) {
    if (NumJumps == MaxNumJumps) // TODO: think about this, maybe print something?
        return;

    NumCandidates = -1; // create a "Reset()" function?
    // TODO: CHECK WHY WE NEED SearchForValue() to receive a pointer anyway, that seems very prone to bugs.
    SearchForValue(Address, 8, 8, true, false);
    printf("NumCandidates for Address %llx: %d\n", *((uint64_t*)Address), NumCandidates);

    int PrevNumPointerNodes = NumPointerNodes;

    for (int I = 0; I < NumCandidates; I++) {
        pointer_node Node = {
            Candidates[I].Address,
            Candidates[I].PointerToCurValue, // TODO: this might to issues because ScanEverything() makes this pointer invalid. Make sure this can never happen!
            (int) (*((uint64_t*)Address) - *((uint64_t*)Candidates[I].PointerToCurValue)),
            NumJumps,
        };

        bool AlreadyMapped = false;
        for (int J = 0; J < PrevNumPointerNodes; J++) {
            if (Node.Address == PointerMap[J].Address) {
                // mark as dead by setting address to 0 (TODO: clean this up)
                AlreadyMapped = true;
            }
        }

        if (!AlreadyMapped) {
            memcpy(&PointerMap[NumPointerNodes++], &Node, sizeof(pointer_node));
        }
    }

    int NewNumPointerNodes = NumPointerNodes;

    // TODO: does this recursion work?
    for (int I = PrevNumPointerNodes; I < NewNumPointerNodes; I++) {
        if (PointerMap[I].Address) {
            GeneratePointerMapRec((char*)&PointerMap[I].Address, NumJumps+1, MaxNumJumps);
        }
    }

    // TODO: filter address not belonging to ".exe" module?
    //       apparently the heap/stack do not belong to a module, so how to deal with this?
    // checkout https://stackoverflow.com/questions/24778487/get-owner-module-from-memory-address
}

// Goes through the memory enumerating possible candidates that point to near the desired address
void GeneratePointerMap(char *Address, int MaxNumJumps) {
    NumPointerNodes = 0;
    GeneratePointerMapRec(Address, 0, MaxNumJumps);
    //TODO: maybe sort results by NumJumps and Address?
    printf("GeneratePointerMap() finished after finding %d nodes.\n", NumPointerNodes);
}

// expects a null terminated input
// returns the length in bytes of the output
int ConvertUtf8ToUtf16(char *Utf8, char *Utf16, int MaxBytes) {
    int MaxUtf16Count = MaxBytes / sizeof(WCHAR);
    int LenInWchar = MultiByteToWideChar(CP_UTF8, 0, Utf8, -1, (LPWSTR) Utf16, MaxUtf16Count);
    return (LenInWchar-1) * sizeof(WCHAR); // we purposefully omit the null terminator
}

// returns the length in bytes of the output
int ConvertUtf16toUtf8(char *Utf16, int LenInBytes, char *Utf8, int MaxBytes) {
    int Utf16Count = LenInBytes / sizeof(WCHAR);
    int ByteCount = WideCharToMultiByte(CP_UTF8, 0, (LPWSTR) Utf16, Utf16Count, Utf8, MaxBytes-1, 0, NULL);
    Utf8[ByteCount] = 0;
    return ByteCount;
}

int main(int argc, char **argv) {

    // IMGUI / GLFW / OpenGL
    ImVec4 clear_color;
    GLFWwindow *window;
    {
        glfwSetErrorCallback(GlfwErrorCallback);
        if (!glfwInit()) return 1;

        // GL 3.0 + GLSL 130
        const char* glsl_version = "#version 130";
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
        //glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);  // 3.2+ only
        //glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);            // 3.0+ only

        // Create window with graphics context
        window = glfwCreateWindow(1280, 720, "Dear ImGui GLFW+OpenGL3 example", NULL, NULL);
        if (window == NULL) return 1;

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1); // Enable vsync

        bool Err = gl3wInit() != 0;
        if (Err) {
            fprintf(stderr, "Failed to initialize OpenGL loader! %d\n", Err);
            return 1;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init(glsl_version);

        // Setup style
        ImGui::StyleColorsDark();
        clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    }

    // TODO: free this properly (and also maybe do something about the magic number)
    //MEMORY_BASIC_INFORMATION32 ProcessData = {};
    ProcessData = (MEMORY_BASIC_INFORMATION *) calloc(MAX_CANDIDATES, sizeof(*ProcessData));
    MemoryBuffer = (char **) calloc(MAX_CANDIDATES, sizeof(*MemoryBuffer));

    double LastTime = glfwGetTime();
    double DeltaTime = 0, NowTime = 0;
    double TimeSinceUpdate = 0;

    bool Attached = false;
    int Searched = MODE_NONE;
    uint64_t AddressToBeLookedup = 0;

    char ProcessIdBuffer[200] = {};
    char ValueToFind[40] = {};
    char AddressToWrite[40] = {};
    char ValueToWrite[40] = {};
    char PointerToBeMapped[40] = {};
    char AddressToBeLookedupText[40] = {};
    char MaxRecursionsText[40] = {};
    while (!glfwWindowShouldClose(window)) {

        // start of frame
        {
            // Poll and handle events (inputs, window resize, etc.)
            // You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
            // - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application.
            // - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application.
            // Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
            glfwPollEvents();

            NowTime = glfwGetTime();
            DeltaTime += (NowTime - LastTime);
            LastTime = NowTime;

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
        }

        // Main Scan Window
        {
            ImGui::SetNextWindowSize(ImVec2(0, 500));
            ImGui::Begin("Scan");
            if (!Attached) {
                ImGui::InputText("Process ID", ProcessIdBuffer, IM_ARRAYSIZE(ProcessIdBuffer));
                if (ImGui::Button("Attach")) {
                    ProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, atoi(ProcessIdBuffer));

#if 1
                    HMODULE Mod;
                    DWORD Needed;
                    if (EnumProcessModules(ProcessHandle, &Mod, sizeof(Mod), &Needed)) {
                        if (Needed > sizeof(Mod)) {
                            // This might be a problem since we need the right module to get the process base address.
                            // Check out https://stackoverflow.com/questions/14467229/get-base-address-of-process
                            //fprintf(stderr, "Needed %d bytes for Module handles array, but got only %zd instead\n", Needed, sizeof(Mod));
                        }
                        char ModName[200];
                        GetModuleFileNameEx(ProcessHandle, Mod, ModName, sizeof(ModName)/sizeof(char));
                        printf("Module name: %s\n", ModName);
                        BaseAddress = (DWORD64) Mod;
                    }
#endif
                    ScanEverything();

                    Attached = true;
                }

#if 1
                DWORD Processes[1024], ProcessesSizeBytes, ProcessesSize;
                if ( !EnumProcesses( Processes, sizeof(Processes), &ProcessesSizeBytes ) ) {
                    fprintf(stderr, "ERROR: Failed to enumerate processes.\n");
                    return NULL;
                }

                for (int I = 0; I < ProcessesSizeBytes / sizeof(DWORD); I++) {
                    HANDLE H = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Processes[I]);
                    if (H) {
                        HMODULE Mod;
                        DWORD Needed;
                        char ProcessName[200];
                        if (EnumProcessModules(H, &Mod, sizeof(Mod), &Needed)) {
                            if (Needed > sizeof(Mod)) {
                                // This might be a problem since we need the right module to get the process base address.
                                // Check out https://stackoverflow.com/questions/14467229/get-base-address-of-process
                                //fprintf(stderr, "Needed %d bytes for Module handles array, but got only %zd instead\n", Needed, sizeof(Mod));
                            }
                            GetModuleBaseName(H, Mod, ProcessName, sizeof(ProcessName)/sizeof(char));
                            ImGui::Text("%6d %s\n", Processes[I], ProcessName);
                        }
                    }
                    CloseHandle(H);
                }
#endif
            } else {
                if (ProcessHandle) {
                    ImGui::Text("Attached Successfully!\n");

#if 1
                    if (NumCandidates > 0 && NumCandidates < 30000) {
                        UpdateCandidates(); //TODO: update only the ones currently appearing on the screen
                    }
#endif

                    ImGui::InputText("Value to find", ValueToFind, IM_ARRAYSIZE(ValueToFind));

#if 1
                    if (ImGui::Button("Find Byte") && strlen(ValueToFind)) {
                        Searched = MODE_BYTE;
                        uint8_t Target = atoi(ValueToFind);
                        SearchForValue((char *) &Target, 1, 1, false, true);
                    }

                    ImGui::SameLine();
#endif


#if 1
                    if (ImGui::Button("Find Short") && strlen(ValueToFind)) {
                        Searched = MODE_SHORT;
                        unsigned short int Target = atoi(ValueToFind);
                        SearchForValue((char *) &Target, 2, 2, false, true);
                    }

                    ImGui::SameLine();
#endif

#if 1
                    if (ImGui::Button("Find Int") && strlen(ValueToFind)) {
                        Searched = MODE_INTEGER;
                        int Target = atoi(ValueToFind);
                        SearchForValue((char *) &Target, sizeof(Target), sizeof(Target), false, true);
                    }

                    ImGui::SameLine();
#endif


#if 1
                    // TODO: handle values starting with "0x", and also handle decimal values
                    if (ImGui::Button("Find Pointer") && strlen(ValueToFind)) {
                        Searched = MODE_POINTER;
                        uint64_t Target = strtoll(ValueToFind, NULL, 16);
                        fprintf(stderr, "Looking for Pointer 0x%I64x\n", Target);
                        SearchForValue((char *) &Target, sizeof(Target), sizeof(Target), true, true);
                    }

                    ImGui::SameLine();
#endif

                    if (ImGui::Button("Find Unicode") && strlen(ValueToFind)) {
                        Searched = MODE_UNICODE;
                        char UnicodeStr[50] = {};
                        int LenInBytes = ConvertUtf8ToUtf16(ValueToFind, UnicodeStr, 50);
                        SearchForValue(UnicodeStr, LenInBytes, 1, false, true);
                    }

#if 1
                    ImGui::SameLine();

                    if (ImGui::Button("Find ASCII") && strlen(ValueToFind)) {
                        Searched = MODE_ASCII;
                        char AsciiStr[50] = {};
                        strcpy(AsciiStr, ValueToFind);
                        SearchForValue(AsciiStr, strlen(AsciiStr), 1, false, true);
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Reset")) {
                        NumCandidates = -1;
                        strcpy(ValueToFind, "");
                    }

                    ImGui::Text("Base address: 0x%p\n", BaseAddress);
                    ImGui::Text("Candidates: %d\n", NumCandidates);

                    // TODO: optimize this for a big number of candidates
                    switch (Searched) {
                        case MODE_NONE:
                            break;
                        case MODE_BYTE:
                            for (int I = 0; I < NumCandidates; I++) {
                                ImGui::Text("%p %u\n", (char *) Candidates[I].Address, *((char *)Candidates[I].PointerToCurValue));
                            }
                            break;
                        case MODE_SHORT:
                            for (int I = 0; I < NumCandidates; I++) {
                                ImGui::Text("%p %d\n", (char *) Candidates[I].Address, *((unsigned short int *)Candidates[I].PointerToCurValue));
                            }
                            break;
                        case MODE_INTEGER:
                            for (int I = 0; I < NumCandidates; I++) {
                                ImGui::Text("%p %p %d\n", (char *) Candidates[I].Address, Candidates[I].PointerToCurValue, *((int *)Candidates[I].PointerToCurValue));
                            }
                            break;
                        case MODE_POINTER:
                            for (int I = 0; I < NumCandidates; I++) {
                                // TODO: handle values starting with "0x", and also handle decimal values
                                // TODO: fix the offset display, it's not suposed to update when we change the "ValueToFind" field
                                uint64_t Target = strtoll(ValueToFind, NULL, 16);
                                ImGui::Text("%p 0x%p 0x%x\n", (char *) Candidates[I].Address, *((uint64_t *)Candidates[I].PointerToCurValue),
                                            Target - *((uint64_t *)Candidates[I].PointerToCurValue));
                            }
                            break;
                        case MODE_UNICODE:
                            for (int I = 0; I < NumCandidates; I++) {
                                char Output[100] = {};
                                ConvertUtf16toUtf8((char *) Candidates[I].PointerToCurValue, Candidates[I].ValueSizeInBytes, Output, 100);

                                char Text[100] = {};
                                sprintf(Text, "%p ", (char *) Candidates[I].Address);
                                int CurSize = strlen(Text);
                                sprintf(Text + CurSize, "%s", Output);
                                CurSize = strlen(Text);

                                ImGui::Text("%s\n", Text);
                            }
                            break;
                        case MODE_ASCII:
                            for (int I = 0; I < NumCandidates; I++) {
                                char Output[100] = {};
                                strncpy(Output, (char *) Candidates[I].PointerToCurValue, Candidates[I].ValueSizeInBytes);

                                char Text[100] = {};
                                sprintf(Text, "%p ", (char *) Candidates[I].Address);
                                int CurSize = strlen(Text);
                                sprintf(Text + CurSize, "%s", Output);
                                CurSize = strlen(Text);

                                ImGui::Text("%s\n", Text);
                            }
                            break;
                        default:
                            assert(false);
                    }
#endif
                } else {
                    ImGui::Text("Failed to attach!\n");
                }
            }
            ImGui::End();
        }

#if 1
        // Writing Window
        if (Attached) {
            ImGui::SetNextWindowSize(ImVec2(0, 0));
            ImGui::Begin("Write");
            ImGui::InputText("Address", AddressToWrite, IM_ARRAYSIZE(AddressToWrite));
            ImGui::InputText("Value", ValueToWrite, IM_ARRAYSIZE(ValueToWrite));

            if (ImGui::Button("Write Int")) {
                int Value = atoi(ValueToWrite);
                int Err = WriteProcessMemory(ProcessHandle, (LPVOID) strtoll(AddressToWrite, NULL, 16), (LPCVOID) &Value, 4, NULL);
                if (!Err) {
                    printf("ERROR writing process memory!\n");
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Write Unicode")) {
                char UnicodeStr[50] = {};
                int LenInBytes = ConvertUtf8ToUtf16(ValueToWrite, UnicodeStr, 50);
                int Err = WriteProcessMemory(ProcessHandle, (LPVOID) strtoll(AddressToWrite, NULL, 16), (LPCVOID) UnicodeStr, LenInBytes, NULL);
                if (!Err) {
                    printf("ERROR writing process memory!\n");
                }
            }

            ImGui::SameLine();

            if (ImGui::Button("Write ASCII")) {
                int Err = WriteProcessMemory(ProcessHandle, (LPVOID) strtoll(AddressToWrite, NULL, 16), (LPCVOID) ValueToWrite, strlen(ValueToWrite), NULL);
                if (!Err) {
                    printf("ERROR writing process memory!\n");
                }
            }

            ImGui::End();
        }
#endif

#if 1
        // Address Lookup Window
        if (Attached) {
            ImGui::SetNextWindowSize(ImVec2(0, 0));
            ImGui::Begin("Address Lookup");

            ImGui::InputText("Address ", AddressToBeLookedupText, IM_ARRAYSIZE(AddressToBeLookedupText));

            // TODO: handle values starting with "0x", and also handle decimal values
            if (ImGui::Button("Search")) {
                AddressToBeLookedup = strtoll(AddressToBeLookedupText, NULL, 16);
            }

            if (AddressToBeLookedup != 0) {
                for (int I = 0; I < NumRegions; I++) {
                    if (AddressToBeLookedup > (uint64_t) ProcessData[I].BaseAddress &&
                        AddressToBeLookedup < (uint64_t) ProcessData[I].BaseAddress + ProcessData[I].RegionSize) {

                        // TODO: refactor this, it's duplicated with ScanEverything() and UpdateCandidates()
                        SIZE_T BytesRead;
                        int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) ProcessData[I].BaseAddress, (LPVOID) MemoryBuffer[I], ProcessData[I].RegionSize, &BytesRead);
                        if (!ReturnValue) {
                            int ErrorValue = GetLastError();
                            //fprintf(stderr, "ERROR: Tried to read from process handle 0x%p at address 0x%p and region size %lld with protection 0x%x\n", (LPVOID) ProcessHandle, (LPVOID) ProcessData[NumRegions].BaseAddress, ProcessData[NumRegions].RegionSize, ProcessData[NumRegions].Protect);
                            //fprintf(stderr, "ERROR: BytesRead %lld\n", BytesRead);
                            // TODO: does this ever happen? If we so, are we handling it correctly?
                            //       Isn't it possible that not incrementing NumRegions here leads to problems? Maybe not. I think this is correct.
                            fprintf(stderr, "ERROR: Problem occurred while reading region. Code: %d\n", ErrorValue); 
                        }

                        char *MirrorAddress = &MemoryBuffer[I][AddressToBeLookedup - (uint64_t) ProcessData[I].BaseAddress];
                        ImGui::Text("Float: %f", *((float *) MirrorAddress));
                        ImGui::Text("Int: %d", *((int *) MirrorAddress));
                        ImGui::Text("Char: %c | %d", *((char *) MirrorAddress), *((uint8_t *) MirrorAddress));
                        ImGui::Text("Pointer: %llx", *((uint64_t *) MirrorAddress));
                        // TODO: ASCII and Unicode
                    }
                }
            }

            ImGui::End();
        }
#endif

#if 1
        // Pointer Map Window
        if (Attached) {
            ImGui::SetNextWindowSize(ImVec2(0, 0));
            ImGui::Begin("Pointer Map");

            ImGui::InputText("Address ", PointerToBeMapped, IM_ARRAYSIZE(PointerToBeMapped));
            ImGui::InputText("MaxRecursions ", MaxRecursionsText, IM_ARRAYSIZE(MaxRecursionsText));


            // TODO: handle values starting with "0x", and also handle decimal values
            if (ImGui::Button("Generate Pointer Map")) {
                uint64_t Target = strtoll(PointerToBeMapped, NULL, 16);
                GeneratePointerMap((char *) &Target, atoi(MaxRecursionsText));
            }

            ImGui::Text("Nodes: %d\n", NumPointerNodes);
            // TODO: optimize this for a big number of candidates
            for (int I = 0; I < NumPointerNodes; I++) {
// TODO: find a good way to reconstruct an optimal path
#if 0
                char OffsetStr[40] = {};
                char TmpStr[40] = {};
                // TODO: optimize this
                for (int J = 0; J < PointerMap[I].NumJumps; J++) {
                    sprintf(TmpStr, "%s0x%03x ", OffsetStr, PointerMap[I].NextJumpOffset);
                    strcpy(OffsetStr, TmpStr);
                }
#endif

                ImGui::Text("%p %llx 0x%03x %d\n", PointerMap[I].Address, *((uint64_t*)PointerMap[I].Value), PointerMap[I].NextJumpOffset, PointerMap[I].NumJumps);
            }

            ImGui::End();
        }
#endif

        // Rendering (end of frame)
        {
            int display_w, display_h;
            glfwMakeContextCurrent(window);
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(clear_color.x, clear_color.y, clear_color.z, clear_color.w);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwMakeContextCurrent(window);
            glfwSwapBuffers(window);
        }
    }

    if (Attached) {
        CloseHandle(ProcessHandle);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    puts("END!"); // DEBUG
    
    return 0;
}
