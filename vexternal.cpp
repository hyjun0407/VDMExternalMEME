#include <iostream>
#include "GL/glew.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#include <Windows.h>
#include <TlHelp32.h>
#include <vector>
#include "offsets.hpp"
#include "vector3.h"
#include "defs.h"
#include <tchar.h>
#include <intrin.h>
#include "libvoyager.hpp"
#include "util/util.hpp"
#include "vdm_ctx/vdm_ctx.hpp"

struct State {
	uintptr_t keys[7];
};

typedef struct {
	uintptr_t actor_ptr;
	uintptr_t damage_handler_ptr;
	uintptr_t player_state_ptr;
	uintptr_t root_component_ptr;
	uintptr_t mesh_ptr;
	uintptr_t bone_array_ptr;
	int bone_count;
	bool is_visible;
} Enemy;

// Window / Process values
HWND valorant_window;
GLFWwindow* g_window;
int g_width;
int g_height;
int g_pid;
uintptr_t g_base_address;
PVOID DllBase;
ImU32 g_esp_color = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 0, 0.4F, 1));
ImU32 g_color_white = ImGui::ColorConvertFloat4ToU32(ImVec4(1, 1, 1, 1));

// Cheat toggle values
bool g_overlay_visible{ false };
bool g_esp_enabled{ true };
bool g_esp_dormantcheck{ false };
bool g_headesp{ true };
bool g_boneesp{ true };
bool g_boxesp{ true };

// Pointers
uintptr_t g_local_player_controller;
uintptr_t g_local_player_pawn;
uintptr_t g_local_damage_handler;
uintptr_t g_camera_manager;
int g_local_team_id;

// Enemy list
std::vector<Enemy> enemy_collection{};

std::wstring s2ws(const std::string& str) {
	int size_needed = MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), NULL, 0);
	std::wstring wstrTo(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, &str[0], (int)str.size(), &wstrTo[0], size_needed);
	return wstrTo;
}

static void glfwErrorCallback(int error, const char* description)
{
	fprintf(stderr, "Glfw Error %d: %s\n", error, description);
}

void setupWindow() {
	glfwSetErrorCallback(glfwErrorCallback);
	if (!glfwInit()) {
		return;
	}

	GLFWmonitor* monitor = glfwGetPrimaryMonitor();
	if (!monitor) {
		return;
	}

	g_width = glfwGetVideoMode(monitor)->width;
	g_height = glfwGetVideoMode(monitor)->height;

	glfwWindowHint(GLFW_FLOATING, true);
	glfwWindowHint(GLFW_RESIZABLE, false);
	glfwWindowHint(GLFW_MAXIMIZED, true);
	glfwWindowHint(GLFW_TRANSPARENT_FRAMEBUFFER, true);

	g_window = glfwCreateWindow(g_width, g_height, "Kgandica", NULL, NULL);
	if (g_window == NULL) {
		return;
	}

	glfwSetWindowAttrib(g_window, GLFW_DECORATED, false);
	glfwSetWindowAttrib(g_window, GLFW_MOUSE_PASSTHROUGH, true);
	glfwSetWindowMonitor(g_window, NULL, 0, 0, g_width, g_height + 1, 0);

	glfwMakeContextCurrent(g_window);
	glfwSwapInterval(1); // Enable vsync

	if (glewInit() != GLEW_OK)
	{
		return;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO(); (void)io;

	ImGui::StyleColorsLight();

	// Setup Platform/Renderer backends
	ImGui_ImplGlfw_InitForOpenGL(g_window, true);
	ImGui_ImplOpenGL3_Init("#version 130");

	ImFont* font = io.Fonts->AddFontFromFileTTF("Roboto-Light.ttf", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
	IM_ASSERT(font != NULL);
}

void cleanupWindow() {
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplGlfw_Shutdown();
	ImGui::DestroyContext();

	glfwDestroyWindow(g_window);
	glfwTerminate();
}

BOOL CALLBACK retreiveValorantWindow(HWND hwnd, LPARAM lparam) {
	DWORD process_id;
	GetWindowThreadProcessId(hwnd, &process_id);
	if (process_id == g_pid) {
		valorant_window = hwnd;
	}
	return TRUE;
}

void activateValorantWindow() {
	SetForegroundWindow(valorant_window);
	mouse_event(MOUSEEVENTF_LEFTDOWN, 0, 0, 0, 0);
	mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0);
}

void handleKeyPresses() {
	// Toggle overlay
	if (GetAsyncKeyState(VK_INSERT) & 1) {
		g_overlay_visible = !g_overlay_visible;
		glfwSetWindowAttrib(g_window, GLFW_MOUSE_PASSTHROUGH, !g_overlay_visible);
		if (g_overlay_visible) {
			HWND overlay_window = glfwGetWin32Window(g_window);
			SetForegroundWindow(overlay_window);
		}
		else {
			activateValorantWindow();
		}
	}
}

uintptr_t decryptWorld(uintptr_t base_address) {
	const auto key = voyager::rpm<uintptr_t>(base_address, voyager::guest_virt_t(reinterpret_cast<uintptr_t>(DllBase) + offsets::key));
	const auto state = voyager::rpm<State>(base_address, voyager::guest_virt_t(reinterpret_cast<uintptr_t>(DllBase) + offsets::state));
	const auto uworld_ptr = decrypt_uworld(key, (uintptr_t*)&state);
	return voyager::rpm<uintptr_t>(base_address, voyager::guest_virt_t(uworld_ptr));
}

struct FText
{
	char _padding_[0x28];
	PWCHAR Name;
	DWORD Length;
};

std::vector<Enemy> retreiveValidEnemies(uintptr_t actor_array, int actor_count) {
	std::vector<Enemy> temp_valorant_dirbase, enemy_collection{};
	size_t size = sizeof(uintptr_t);
	for (int i = 0; i < actor_count; i++) {
		uintptr_t actor = voyager::rpm<uintptr_t>(g_base_address, actor_array + (i * size));
		if (actor == 0x00) {
			continue;
		}
		uintptr_t unique_id = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(actor + offsets::camera::unique_id));
		if (unique_id != 18743553) {
			continue;
		}
		uintptr_t mesh = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(actor + offsets::camera::mesh_component));
		if (!mesh) {
			continue;
		}

		uintptr_t player_state = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(actor + offsets::camera::player_state));
		uintptr_t team_component = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(player_state + offsets::camera::team_component));
		int team_id = voyager::rpm<int>(g_base_address, voyager::guest_virt_t(team_component + offsets::camera::team_id));
		int bone_count = voyager::rpm<int>(g_base_address, voyager::guest_virt_t(mesh + offsets::camera::bone_count));
		bool is_bot = bone_count == 103;
		if (team_id == g_local_team_id && !is_bot) {
			continue;
		}

		uintptr_t damage_handler = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(actor + offsets::camera::damage_handler));
		uintptr_t root_component = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(actor + offsets::camera::root_component));
		uintptr_t bone_array = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(mesh + offsets::camera::bone_array));

		Enemy enemy{
			actor,
			damage_handler,
			player_state,
			root_component,
			mesh,
			bone_array,
			bone_count,
			true
		};

		temp_valorant_dirbase, enemy_collection.push_back(enemy);
	}

	return temp_valorant_dirbase, enemy_collection;
}

void retreiveData() {
	while (true) {
		uintptr_t world = decryptWorld(g_base_address);

		const auto persistent_level = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(world + offsets::camera::persistent_level));
		const auto game_instance = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(world + offsets::camera::game_instance));
		const auto local_player_array = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(game_instance + offsets::camera::local_player_array));
		const auto local_player = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player_array));
		const auto local_player_controller = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player + offsets::camera::local_player_controller));
		const auto local_player_pawn = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player_controller + offsets::camera::local_player_pawn));
		const auto local_damage_handler = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player_pawn + offsets::camera::damage_handler));
		const auto local_player_state = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player_pawn + offsets::camera::player_state));
		const auto local_team_component = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player_state + offsets::camera::team_component));
		static int local_team_id = voyager::rpm<int>(g_base_address, voyager::guest_virt_t(local_team_component + offsets::camera::team_id));

		const auto camera_manager = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(local_player_controller + offsets::camera::camera_manager));

		const auto actor_array = voyager::rpm<uintptr_t>(g_base_address, voyager::guest_virt_t(persistent_level + offsets::camera::actor_array));
		static int actor_count = voyager::rpm<int>(g_base_address, voyager::guest_virt_t(persistent_level + offsets::camera::actor_count));
		g_local_player_controller = local_player_controller;
		g_local_player_pawn = local_player_pawn;
		g_local_damage_handler = local_damage_handler;
		g_camera_manager = camera_manager;
		g_local_team_id = local_team_id;

		enemy_collection = retreiveValidEnemies(actor_array, actor_count);
		Sleep(2500);
	}
}

Vector3 getBonePosition(Enemy enemy, int index) {
	size_t size = sizeof(FTransform);
	FTransform firstBone = voyager::rpm<FTransform>(g_base_address, voyager::guest_virt_t(enemy.bone_array_ptr + (size * index)));
	FTransform componentToWorld = voyager::rpm<FTransform>(g_base_address, voyager::guest_virt_t(enemy.mesh_ptr + offsets::camera::component_to_world));
	D3DMATRIX matrix = MatrixMultiplication(firstBone.ToMatrixWithScale(), componentToWorld.ToMatrixWithScale());
	return Vector3(matrix._41, matrix._42, matrix._43);
}

void renderBoneLine(Vector3 first_bone_position, Vector3 second_bone_position, Vector3 position, Vector3 rotation, float fov) {
	Vector2 first_bone_screen_position = worldToScreen(first_bone_position, position, rotation, fov);
	ImVec2 fist_screen_position = ImVec2(first_bone_screen_position.x, first_bone_screen_position.y);
	Vector2 second_bone_screen_position = worldToScreen(second_bone_position, position, rotation, fov);
	ImVec2 second_screen_position = ImVec2(second_bone_screen_position.x, second_bone_screen_position.y);
	ImGui::GetOverlayDrawList()->AddLine(fist_screen_position, second_screen_position, g_color_white);
}

void renderBones(Enemy enemy, Vector3 position, Vector3 rotation, float fov) {
	Vector3 head_position = getBonePosition(enemy, 8);
	Vector3 neck_position;
	Vector3 chest_position = getBonePosition(enemy, 6);
	Vector3 l_upper_arm_position;
	Vector3 l_fore_arm_position;
	Vector3 l_hand_position;
	Vector3 r_upper_arm_position;
	Vector3 r_fore_arm_position;
	Vector3 r_hand_position;
	Vector3 stomach_position = getBonePosition(enemy, 4);
	Vector3 pelvis_position = getBonePosition(enemy, 3);
	Vector3 l_thigh_position;
	Vector3 l_knee_position;
	Vector3 l_foot_position;
	Vector3 r_thigh_position;
	Vector3 r_knee_position;
	Vector3 r_foot_position;
	if (enemy.bone_count == 102) { // MALE
		neck_position = getBonePosition(enemy, 19);

		l_upper_arm_position = getBonePosition(enemy, 21);
		l_fore_arm_position = getBonePosition(enemy, 22);
		l_hand_position = getBonePosition(enemy, 23);

		r_upper_arm_position = getBonePosition(enemy, 47);
		r_fore_arm_position = getBonePosition(enemy, 48);
		r_hand_position = getBonePosition(enemy, 49);

		l_thigh_position = getBonePosition(enemy, 75);
		l_knee_position = getBonePosition(enemy, 76);
		l_foot_position = getBonePosition(enemy, 78);

		r_thigh_position = getBonePosition(enemy, 82);
		r_knee_position = getBonePosition(enemy, 83);
		r_foot_position = getBonePosition(enemy, 85);
	}
	else if (enemy.bone_count == 99) { // FEMALE
		neck_position = getBonePosition(enemy, 19);

		l_upper_arm_position = getBonePosition(enemy, 21);
		l_fore_arm_position = getBonePosition(enemy, 40);
		l_hand_position = getBonePosition(enemy, 42);

		r_upper_arm_position = getBonePosition(enemy, 46);
		r_fore_arm_position = getBonePosition(enemy, 65);
		r_hand_position = getBonePosition(enemy, 67);

		l_thigh_position = getBonePosition(enemy, 78);
		l_knee_position = getBonePosition(enemy, 75);
		l_foot_position = getBonePosition(enemy, 77);

		r_thigh_position = getBonePosition(enemy, 80);
		r_knee_position = getBonePosition(enemy, 82);
		r_foot_position = getBonePosition(enemy, 84);
	}
	else if (enemy.bone_count == 103) { // BOT
		neck_position = getBonePosition(enemy, 9);

		l_upper_arm_position = getBonePosition(enemy, 33);
		l_fore_arm_position = getBonePosition(enemy, 30);
		l_hand_position = getBonePosition(enemy, 32);

		r_upper_arm_position = getBonePosition(enemy, 58);
		r_fore_arm_position = getBonePosition(enemy, 55);
		r_hand_position = getBonePosition(enemy, 57);

		l_thigh_position = getBonePosition(enemy, 63);
		l_knee_position = getBonePosition(enemy, 65);
		l_foot_position = getBonePosition(enemy, 69);

		r_thigh_position = getBonePosition(enemy, 77);
		r_knee_position = getBonePosition(enemy, 79);
		r_foot_position = getBonePosition(enemy, 83);
	}
	else {
		return;
	}

	renderBoneLine(head_position, neck_position, position, rotation, fov);
	renderBoneLine(neck_position, chest_position, position, rotation, fov);
	renderBoneLine(neck_position, l_upper_arm_position, position, rotation, fov);
	renderBoneLine(l_upper_arm_position, l_fore_arm_position, position, rotation, fov);
	renderBoneLine(l_fore_arm_position, l_hand_position, position, rotation, fov);
	renderBoneLine(neck_position, r_upper_arm_position, position, rotation, fov);
	renderBoneLine(r_upper_arm_position, r_fore_arm_position, position, rotation, fov);
	renderBoneLine(r_fore_arm_position, r_hand_position, position, rotation, fov);
	renderBoneLine(chest_position, stomach_position, position, rotation, fov);
	renderBoneLine(stomach_position, pelvis_position, position, rotation, fov);
	renderBoneLine(pelvis_position, l_thigh_position, position, rotation, fov);
	renderBoneLine(l_thigh_position, l_knee_position, position, rotation, fov);
	renderBoneLine(l_knee_position, l_foot_position, position, rotation, fov);
	renderBoneLine(pelvis_position, r_thigh_position, position, rotation, fov);
	renderBoneLine(r_thigh_position, r_knee_position, position, rotation, fov);
	renderBoneLine(r_knee_position, r_foot_position, position, rotation, fov);
}

void renderBox(Vector2 head_at_screen, float distance_modifier) {
	int head_x = head_at_screen.x;
	int head_y = head_at_screen.y;
	int start_x = head_x - 35 / distance_modifier;
	int start_y = head_y - 15 / distance_modifier;
	int end_x = head_x + 35 / distance_modifier;
	int end_y = head_y + 155 / distance_modifier;
	ImGui::GetOverlayDrawList()->AddRect(ImVec2(start_x, start_y), ImVec2(end_x, end_y), g_esp_color);
}

static char CurrDir[MAX_PATH] = "";

typedef int (FAR WINAPI* pDD_btn)(int btn);
typedef int (FAR WINAPI* pDD_key)(int keycode, int flag);
typedef int (FAR WINAPI* pDD_movR)(int dx, int dy);

static pDD_btn  DD_btn;
static pDD_key  DD_key;
static pDD_movR DD_movR;

int TarGetIndex = -1;

void renderEsp() {
	std::vector<Enemy> local_enemy_collection = enemy_collection;
	if (local_enemy_collection.empty()) {
		return;
	}

	Vector3 camera_position = voyager::rpm<Vector3>(g_base_address, voyager::guest_virt_t(g_camera_manager + offsets::camera::camera_position));
	Vector3 camera_rotation = voyager::rpm<Vector3>(g_base_address, voyager::guest_virt_t(g_camera_manager + offsets::camera::camera_rotation));
	float camera_fov = voyager::rpm<float>(g_base_address, voyager::guest_virt_t(g_camera_manager + offsets::camera::camera_fov));

	for (int i = 0; i < local_enemy_collection.size(); i++) {
		Enemy enemy = enemy_collection[i];
		float health = voyager::rpm<float>(g_base_address, voyager::guest_virt_t(enemy.damage_handler_ptr + offsets::camera::health));
		if (enemy.actor_ptr == g_local_player_pawn || health <= 0 || !enemy.mesh_ptr) {
			continue;
		}

		Vector3 head_position = getBonePosition(enemy, 8); // 8 = head bone
		Vector3 root_position = voyager::rpm<Vector3>(g_base_address, voyager::guest_virt_t(enemy.root_component_ptr + offsets::camera::root_position));
		if (head_position.z <= root_position.z) {
			continue;
		}

		if (g_esp_dormantcheck) {
			bool dormant = voyager::rpm<bool>(g_base_address, voyager::guest_virt_t(enemy.actor_ptr + offsets::camera::dormant));
			if (!dormant) {
				continue;
			}
		}

		Vector2 head_at_screen_vec = worldToScreen(head_position, camera_position, camera_rotation, camera_fov);
		
		ImVec2 head_at_screen = ImVec2(head_at_screen_vec.x, head_at_screen_vec.y);
		float distance_modifier = camera_position.Distance(head_position) * 0.001F;

		if (g_boneesp) {
			renderBones(enemy, camera_position, camera_rotation, camera_fov);
		}
		if (g_headesp) {
			ImGui::GetOverlayDrawList()->AddCircle(head_at_screen, 7 / distance_modifier, g_esp_color, 0, 3);
		}
		if (g_boxesp) {
			renderBox(head_at_screen_vec, distance_modifier);
		}
	}
}

void runRenderTick() {
	glfwPollEvents();

	ImGui_ImplOpenGL3_NewFrame();
	ImGui_ImplGlfw_NewFrame();
	ImGui::NewFrame();

	if (g_esp_enabled) {
		renderEsp();
	}

	if (g_overlay_visible) {
		// Visuals Window
		{
			ImGui::Begin("vs", nullptr, ImGuiWindowFlags_NoResize);
			ImGui::SetWindowSize("vs", ImVec2(200, 176));

			ImGui::Checkbox("ep", &g_esp_enabled);
			ImGui::Checkbox("epdrm", &g_esp_dormantcheck);
			ImGui::Checkbox("HE", &g_headesp);
			ImGui::Checkbox("BE", &g_boneesp);
			ImGui::Checkbox("BOE", &g_boxesp);
			ImGui::End();
		}
	}

	ImGui::Render();
	int display_w, display_h;
	glfwGetFramebufferSize(g_window, &display_w, &display_h);
	glViewport(0, 0, display_w, display_h);
	glClear(GL_COLOR_BUFFER_BIT);
	ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

	glfwSwapBuffers(g_window);
}

int main()
{

	auto voyager_init = voyager::init();
	if (voyager_init != voyager::vmxroot_error_t::error_success) {
		exit(0);
	}

	g_pid = util::get_pid("VALORANT-Win64-Shipping.exe");
	if (g_pid == 0) {
		exit(0);
	}

	vdm::read_phys_t _read_phys =
		[&](void* addr, void* buffer, std::size_t size) -> bool
	{
		const auto read_result =
			voyager::read_phys((u64)addr, (u64)buffer, size);

		return read_result ==
			voyager::vmxroot_error_t::error_success;
	};

	vdm::write_phys_t _write_phys =
		[&](void* addr, void* buffer, std::size_t size) -> bool
	{
		const auto write_result =
			voyager::write_phys((u64)addr, (u64)buffer, size);

		return write_result ==
			voyager::vmxroot_error_t::error_success;
	};

	vdm::vdm_ctx vdm(_read_phys, _write_phys);
	g_base_address = vdm.get_dirbase(g_pid);
	auto peb_kernel = vdm.get_peb(g_pid);

	const auto peb = voyager::rpm<PEB>(g_base_address, reinterpret_cast<voyager::guest_virt_t>(peb_kernel));
	const auto ldr = voyager::rpm<PEB_LDR_DATA>(g_base_address, reinterpret_cast<voyager::guest_virt_t>(peb.Ldr));

	LDR_DATA_TABLE_ENTRY entry = voyager::rpm<LDR_DATA_TABLE_ENTRY>(g_base_address, reinterpret_cast<voyager::guest_virt_t>(ldr.InMemoryOrderModuleList.Flink) - sizeof LIST_ENTRY);

	const auto module_name = voyager::rpm_buffer<WCHAR>(g_base_address, reinterpret_cast<voyager::guest_virt_t>(entry.FullDllName.Buffer), entry.FullDllName.Length);
	DllBase = entry.DllBase;
	// Get the valorant game window
	EnumWindows(retreiveValorantWindow, NULL);
	if (!valorant_window) {
		system("pause");
		return 1;
	}

	if (!g_base_address) {
		system("pause");
		return 1;
	}

	// Create the opengl overlay window and setup imgui
	setupWindow();
	if (!g_window) {
		system("pause");
		return 1;
	}

	// Retreive data loop thread
	HANDLE handle = CreateThread(nullptr, NULL, (LPTHREAD_START_ROUTINE)retreiveData, nullptr, NULL, nullptr);
	if (handle) {
		CloseHandle(handle);
	}

	// Main loop
	while (!glfwWindowShouldClose(g_window))
	{
		handleKeyPresses();
		runRenderTick();
	}

	// Cleanup
	// Cleanup
	cleanupWindow();
}
