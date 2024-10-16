#include "gui.hpp"

namespace zrx {
GuiRenderer::GuiRenderer(GLFWwindow *w, ImGui_ImplVulkan_InitInfo &imguiInitInfo) : window(w) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOpenGL(window, true);

    ImGui_ImplVulkan_Init(&imguiInitInfo, nullptr);

    imguiGizmo::setGizmoFeelingRot(0.3);
}

GuiRenderer::~GuiRenderer() {
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

void GuiRenderer::beginRendering() {
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
                                       | ImGuiWindowFlags_NoCollapse
                                       | ImGuiWindowFlags_NoSavedSettings
                                       | ImGuiWindowFlags_NoResize
                                       | ImGuiWindowFlags_NoMove;

    ImGui::SetNextWindowPos(ImVec2(0, 0));

    glm::ivec2 windowSize;
    glfwGetWindowSize(window, &windowSize.x, &windowSize.y);
    ImGui::SetNextWindowSize(ImVec2(0, windowSize.y));

    ImGui::Begin("main window", nullptr, flags);
}

void GuiRenderer::endRendering(const vk::raii::CommandBuffer &commandBuffer) {
    ImGui::End();
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), *commandBuffer);
}
}
