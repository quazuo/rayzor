#pragma once

#include "libs.hpp"
#include "globals.hpp"
#include "src/utils/input-manager.hpp"

struct GLFWwindow;

namespace zrx {
class Rotator {
    glm::vec2 rot = {0, 0};

public:
    [[nodiscard]] glm::vec2 operator*() const { return rot; }

    Rotator& operator=(glm::vec2 other);

    Rotator& operator+=(glm::vec2 other);

    Rotator& operator-=(glm::vec2 other);

    struct ViewVectors {
        glm::vec3 front, right, up;
    };

    [[nodiscard]] ViewVectors getViewVectors() const;
};

class Camera {
    GLFWwindow *window = nullptr;

    float aspectRatio = 4.0f / 3.0f;
    float fieldOfView = 80.0f;
    float zNear = 0.01f;
    float zFar = 500.0f;

    glm::vec3 pos = {0.0f, 0.0f, -2.0f};
    Rotator rotator;
    glm::vec3 front{}, right{}, up{};

    bool isLockedCursor = false;
    bool isLockedCam = true;
    float lockedRadius = 2.0f;
    Rotator lockedRotator;

    float rotationSpeed = 2.5f;
    float movementSpeed = 5.0f;

    unique_ptr<InputManager> inputManager;

public:
    explicit Camera(GLFWwindow *w);

    void tick(float deltaTime);

    [[nodiscard]] glm::vec3 getPos() const { return pos; }

    [[nodiscard]] glm::mat4 getViewMatrix() const;

    [[nodiscard]] glm::mat4 getStaticViewMatrix() const;

    [[nodiscard]] glm::mat4 getProjectionMatrix() const;

    [[nodiscard]] Rotator::ViewVectors getViewVectors() const { return rotator.getViewVectors(); }

    [[nodiscard]] std::pair<float, float> getClippingPlanes() const { return {zNear, zFar}; }

    void renderGuiSection();

private:
    static void scrollCallback(GLFWwindow *window, double dx, double dy);

    void bindCameraLockKey();

    /**
     * Binds keys used to rotate the camera.
     */
    void bindMouseDragCallback();

    /**
     * Binds keys used to rotate the camera in freecam mode.
     */
    void bindFreecamRotationKeys();

    /**
     * Binds keys used to move the camera in freecam mode.
     */
    void bindFreecamMovementKeys();

    void tickMouseMovement(float deltaTime);

    void tickLockedMode();

    void updateAspectRatio();

    void updateVecs();

    void centerCursor() const;
};
} // zrx
