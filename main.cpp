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

#define MEGABYTES(a) (a * 1000000)
#define WRITABLE (PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)

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
struct CandidateInfo {
    unsigned char *Address;
    int *PointerToCurValue;
};

// Array storing all current candidates
CandidateInfo *Candidates = (CandidateInfo *) calloc(20000, sizeof(*Candidates));
int NumCandidates = -1;


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
        //printf("0x%p %lld\n", ProcessData[I].BaseAddress, ProcessData[I].RegionSize);
        ImGui::Text("0x%p %lld\n", ProcessData[I].BaseAddress, ProcessData[I].RegionSize);
    }
}

// If there are no candidates, scans the whole memory and build a list of matching candidates
// Otherwise, scans the candidates list to filter it using a new value
void SearchForValue(int Value, int Stride = 4) { // word by word by default
    //printf("Value %d\n", Value);
    //printf("NumCandidates %d\n", NumCandidates); // DEBUG
    if (NumCandidates == -1) { // TODO: redo this, its a dirty hack for now
        for (int I = 0; I < NumRegions; I++) {
            for (int J = 0; J < ProcessData[I].RegionSize; J+=Stride) {
                int Candidate = *((int *) &MemoryBuffer[I][J]);
                if (Candidate == Value) {
                    Candidates[NumCandidates].Address = (unsigned char *)ProcessData[I].BaseAddress + J;
                    Candidates[NumCandidates].PointerToCurValue = (int *) &MemoryBuffer[I][J];
                    NumCandidates++;
                }
            }
        }
    } else {
        for (int I = 0; I < NumCandidates;) {
            if (*Candidates[I].PointerToCurValue != Value) {
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

// If there are no candidates, scans the whole memory and build a list of matching candidates
// Otherwise, scans the candidates list to filter it using a new value
void SearchForString(int Value) {
    //printf("Value %d\n", Value);
    //printf("NumCandidates %d\n", NumCandidates); // DEBUG
    if (NumCandidates == -1) { // TODO: redo this, its a dirty hack for now
        for (int I = 0; I < NumRegions; I++) {
            for (int J = 0; J < ProcessData[I].RegionSize; J+=4) { // word by word
                int Candidate = *((int *) &MemoryBuffer[I][J]);
                if (Candidate == Value) {
                    Candidates[NumCandidates].Address = (unsigned char *)ProcessData[I].BaseAddress + J;
                    Candidates[NumCandidates].PointerToCurValue = (int *) &MemoryBuffer[I][J];
                    NumCandidates++;
                }
            }
        }
    } else {
        for (int I = 0; I < NumCandidates;) {
            if (*Candidates[I].PointerToCurValue != Value) {
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
        int ReturnValue = ReadProcessMemory(ProcessHandle, (LPCVOID) Candidates[I].Address, (LPVOID) Candidates[I].PointerToCurValue, 4, &BytesRead);
        if (!ReturnValue) {
            int ErrorValue = GetLastError();
            // TODO
            //printf("ERROR: Problem occurred while reading region. Code: %d\n", ErrorValue);
        }
    }
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
        if (window == NULL)
            return 1;
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
    bool Searched = false;

    char ProcessIdBuffer[200] = {};
    char NumberToFind[40] = {};
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

                int ErrorValue = GetLastError();
                if (ErrorValue) {
                    printf("ERROR1?: %d\n", ErrorValue);
                }

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

                    ImGui::InputText("Number to find", NumberToFind, IM_ARRAYSIZE(NumberToFind));
                    if (ImGui::Button("Search")) { // TODO: fix a bug where application hangs when user presses Search with no number typed
                        Searched = true;
                        SearchForValue(atoi(NumberToFind));
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Reset")) {
                        NumCandidates = -1;
                        strcpy(NumberToFind, "");
                    }
                    if (Searched) {
                        for (int I = 0; I < NumCandidates; I++) {
                            ImGui::Text("%p %d\n", (unsigned char *) Candidates[I].Address, *Candidates[I].PointerToCurValue);
                        }
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
            if (ImGui::Button("Write")) {
                int Value = atoi(ValueToWrite);
                int Err = WriteProcessMemory(ProcessHandle, (LPVOID) strtoll(AddressToWrite, NULL, 16), (LPCVOID) &Value, 4, NULL);
                printf("ERR: %d\n", Err);
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
