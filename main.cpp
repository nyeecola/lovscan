#define NTDDI_VERSION NTDDI_WIN7
#define _WIN32_WINNT _WIN32_WINNT_WIN7

#include <afxwin.h>
#include <Psapi.h>
#include <windows.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

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

// TODO; fix all magic numbers

//
// DATA
//

// Array storing information about all sections of process memory that we care about (WRITABLE && COMMITTED)
MEMORY_BASIC_INFORMATION *ProcessData;

// Array storing data from all sections of process memory that we care about
unsigned char **MemoryBuffer;

// Total number of process memory sections that we care about
int NumRegions;

// "Attached" process handle
HANDLE ProcessHandle;

// Struct storing information about a matching candidate
struct candidate_info {
    unsigned char *Address;
    void *PointerToCurValue;
    int ValueSizeInBytes;
};

// Array storing all current candidates
candidate_info *Candidates = (candidate_info *) calloc(20000, sizeof(*Candidates));
int NumCandidates = -1;

enum search_mode {
    NONE,
    INTEGER,
    UNICODE,
    ASCII
};


//
// MAIN PROGRAM
//

static void GlfwErrorCallback(int error, const char* description)
{
    fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

// UNUSED
void DumpMemoryRegionsInfo() {
    for (int I = 0; I < NumRegions; I++) {
        ImGui::Text("0x%p %lld\n", ProcessData[I].BaseAddress, ProcessData[I].RegionSize);
    }
}

// If there are no candidates, scans the whole memory and build a list of matching candidates
// Otherwise, scans the candidates list to filter it using a new value
void SearchForValue(char *Data, int Len, int Stride) {
    if (NumCandidates == -1) { // TODO: redo this, its a dirty hack for now
        for (int I = 0; I < NumRegions; I++) {
            for (int J = 0; J < ProcessData[I].RegionSize - Len; J+=Stride) {
                char *Candidate = (char *) &MemoryBuffer[I][J];
                if (!memcmp(Candidate, Data, Len)) {
                    Candidates[NumCandidates].Address = (unsigned char *)ProcessData[I].BaseAddress + J;
                    Candidates[NumCandidates].PointerToCurValue = (void *) &MemoryBuffer[I][J];
                    Candidates[NumCandidates].ValueSizeInBytes = Len;
                    NumCandidates++;
                }
            }
        }
    } else {
        for (int I = 0; I < NumCandidates;) {
            if (memcmp((char *)Candidates[I].PointerToCurValue, Data, Len)) {
                for (int J = I; J < NumCandidates-1; J++) {
                    Candidates[J] = Candidates[J+1];
                }
                NumCandidates--;
            } else {
                I++;
            }
        }
    }
}

// Scan the whole memory and builds an array of memory regions
void ScanEverything() {
    unsigned char *Addr = 0;
    for (NumRegions = 0;;) {
        if (VirtualQueryEx(ProcessHandle, Addr, &ProcessData[NumRegions], sizeof(ProcessData[NumRegions])) == 0) {
            printf("Finished querying process memory.\nFound %d writable regions.\n", NumRegions);
            break;
        }

        Addr = (unsigned char *)ProcessData[NumRegions].BaseAddress + ProcessData[NumRegions].RegionSize;

        // TODO: figure out which pages arent being filtered correctly (we're getting some error codes 299 on ReadProcessMemory)
        if ((ProcessData[NumRegions].State & MEM_COMMIT) && (ProcessData[NumRegions].Protect & WRITABLE)) {
            MemoryBuffer[NumRegions] = (unsigned char *) calloc(ProcessData[NumRegions].RegionSize, sizeof(char));
            SIZE_T BytesRead;
            int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) ProcessData[NumRegions].BaseAddress, (LPVOID) MemoryBuffer[NumRegions], ProcessData[NumRegions].RegionSize, &BytesRead);
            if (!ReturnValue) {
                int ErrorValue = GetLastError();
                // TODO
                //printf("ERROR: Problem occurred while reading region. Code: %d\n", ErrorValue);
            }

            NumRegions++;
        }
    }
}

// TODO: Optimize this
// Reads every single region again (doesn't discover new regions)
void UpdateEverything() {
    for (int I = 0; I < NumRegions; I++) {
        SIZE_T BytesRead;
        int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) ProcessData[I].BaseAddress, (LPVOID) MemoryBuffer[I], ProcessData[I].RegionSize, &BytesRead);
        if (!ReturnValue) {
            int ErrorValue = GetLastError();
            // TODO
            //printf("ERROR: Problem occurred while updating region. Code: %d\n", ErrorValue);
        }
    }
}

// Reads every single candidate again (doesn't add new candidates)
void UpdateCandidates() {
    for (int I = 0; I < NumCandidates; I++) {
        SIZE_T BytesRead;
        int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) Candidates[I].Address, (LPVOID) Candidates[I].PointerToCurValue, Candidates[I].ValueSizeInBytes, &BytesRead);
        if (!ReturnValue) {
            int ErrorValue = GetLastError();
            printf("ERROR: Problem occurred while reading region. Code: %d\n", ErrorValue);
        }
    }
}

// expects a null terminated input
// returns the length in bytes of the output
int ConvertAsciiToUtf8(char *Ascii, char *Utf8) {
    int LenInWchar = MultiByteToWideChar(CP_UTF8, 0, Ascii, strlen(Ascii), NULL, 0);
    MultiByteToWideChar(CP_UTF8, 0, Ascii, strlen(Ascii), (LPWSTR) Utf8, LenInWchar);
    return LenInWchar * 2;
}

// expects a null terminated input
// returns the length in bytes of the output
int ConvertUtf8ToAscii(char *Utf8, int LenInBytes, char *Ascii) {
    WideCharToMultiByte(CP_ACP, 0, (LPWSTR) Utf8, LenInBytes / 2, Ascii, LenInBytes / 2, 0, NULL);
    return LenInBytes / 2;
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
        // TODO
        //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init(glsl_version);

        // Setup style
        ImGui::StyleColorsDark();
        clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);
    }

    // TODO: free this properly (and also maybe do something about the magic number)
    //MEMORY_BASIC_INFORMATION32 ProcessData = {};
    ProcessData = (MEMORY_BASIC_INFORMATION *) calloc(20000, sizeof(*ProcessData));
    MemoryBuffer = (unsigned char **) calloc(20000, sizeof(*MemoryBuffer));

    double LastTime = glfwGetTime();
    double DeltaTime = 0, NowTime = 0;
    double TimeSinceUpdate = 0;

    bool Attached = false;
    int Searched = NONE;

    char ProcessIdBuffer[200] = {};
    char ValueToFind[40] = {};
    char AddressToWrite[40] = {};
    char ValueToWrite[40] = {};
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

        // Main Window
        {
            ImGui::SetNextWindowSize(ImVec2(0, 500));
            ImGui::Begin("Main Window");
            if (!Attached) {
                ImGui::InputText("Process ID", ProcessIdBuffer, IM_ARRAYSIZE(ProcessIdBuffer));
                if (ImGui::Button("Attach")) {
                    ProcessHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, atoi(ProcessIdBuffer));
                    ScanEverything();
                    Attached = true;
                }

                DWORD Processes[1024], ProcessesSizeBytes, ProcessesSize;
                if ( !EnumProcesses( Processes, sizeof(Processes), &ProcessesSizeBytes ) )
                    return NULL;

                for (int I = 0; I < ProcessesSizeBytes / sizeof(DWORD); I++) {
                    HANDLE H = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, Processes[I]);
                    if (H) {
                        HMODULE Mod;
                        DWORD Needed;
                        char ProcessName[200];
                        if (EnumProcessModules(H, &Mod, sizeof(Mod), &Needed)) {
                            GetModuleBaseName(H, Mod, ProcessName, sizeof(ProcessName)/sizeof(char));
                            ImGui::Text("%6d %s\n", Processes[I], ProcessName);
                        }
                    }
                }
            } else {
                if (ProcessHandle) {
                    ImGui::Text("Attached Successfully!\n");
                    // TODO: Optimize all of this

                    if (NumCandidates != -1) {
                        UpdateCandidates();
                    } else {
                        TimeSinceUpdate += DeltaTime;
                        if (TimeSinceUpdate > 3) { // TODO: lower this once it is optimized
                            UpdateEverything();
                            TimeSinceUpdate = 0;
                        }
                    }

                    ImGui::InputText("Value to find", ValueToFind, IM_ARRAYSIZE(ValueToFind));
                    if (ImGui::Button("Find Int") && strlen(ValueToFind)) {
                        Searched = INTEGER;
                        int Target = atoi(ValueToFind);
                        SearchForValue((char *) &Target, sizeof(Target), sizeof(Target));
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Find Unicode") && strlen(ValueToFind)) {
                        Searched = UNICODE;
                        char UnicodeStr[50] = {};
                        int LenInBytes = ConvertAsciiToUtf8(ValueToFind, UnicodeStr);
                        SearchForValue(UnicodeStr, LenInBytes, 1);
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Find ASCII") && strlen(ValueToFind)) {
                        Searched = ASCII;
                        SearchForValue(ValueToFind, strlen(ValueToFind), 1);
                    }

                    ImGui::SameLine();

                    if (ImGui::Button("Reset")) {
                        NumCandidates = -1;
                        strcpy(ValueToFind, "");
                    }

                    switch (Searched) {
                        case NONE:
                            break;
                        case INTEGER:
                            for (int I = 0; I < NumCandidates; I++) {
                                ImGui::Text("%p %d\n", (unsigned char *) Candidates[I].Address, *((int *)Candidates[I].PointerToCurValue));
                            }
                            break;
                        case UNICODE:
                            for (int I = 0; I < NumCandidates; I++) {
                                char Output[100] = {};
                                ConvertUtf8ToAscii((char *) Candidates[I].PointerToCurValue, Candidates[I].ValueSizeInBytes, Output);

                                char Text[100] = {};
                                sprintf(Text, "%p ", (unsigned char *) Candidates[I].Address);
                                int CurSize = strlen(Text);
                                sprintf(Text + CurSize, "%s", Output);
                                CurSize = strlen(Text);

                                ImGui::Text("%s\n", Text);
                            }
                            break;
                        case ASCII:
                            for (int I = 0; I < NumCandidates; I++) {
                                char Output[100] = {};
                                strncpy(Output, (char *) Candidates[I].PointerToCurValue, Candidates[I].ValueSizeInBytes);

                                char Text[100] = {};
                                sprintf(Text, "%p ", (unsigned char *) Candidates[I].Address);
                                int CurSize = strlen(Text);
                                sprintf(Text + CurSize, "%s", Output);
                                CurSize = strlen(Text);

                                ImGui::Text("%s\n", Text);
                            }
                            break;
                        default:
                            assert(false);
                    }
                } else {
                    ImGui::Text("Failed to attach!\n");
                }
            }
            ImGui::End();
        }

        // Writing Window
        {
            ImGui::SetNextWindowSize(ImVec2(0, 0));
            ImGui::Begin("Writing Window");
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
                int LenInBytes = ConvertAsciiToUtf8(ValueToWrite, UnicodeStr);
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
    
    return 0;
}
