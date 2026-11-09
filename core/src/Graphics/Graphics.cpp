#include "StdInclude.hpp"
#include "Graphics.hpp"

#include <bit>

#include "UI/UIManager.hpp"
#include "Components/CameraManager.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/CampathManager.hpp"
#include "Components/KillstreakLogic.hpp"
#include "Components/Actors.hpp"
#include "Components/LightLogic.hpp"
#include "Components/Lights.hpp"
#include "Components/PropLogic.hpp"
#include "Components/Props.hpp"
#include "Components/Playback.hpp"
#include "Configuration/PreferencesConfiguration.hpp"
#include "Graphics/Resource.hpp"
#include "Graphics/ViewLines.hpp"
#include "Input.hpp"
#include "Mod.hpp"
#include "Types/Vertex.hpp"
#include "Utilities/MathUtils.hpp"
#include "UI/Components/GameView.hpp"
#include "UI/ActorActions.hpp"
#include "UI/GaitColors.hpp"

#include "imgui_internal.h"

INCBIN_EXTERN(VERTEX_SHADER);
INCBIN_EXTERN(PIXEL_SHADER);
INCBIN_EXTERN(DEPTH_VERTEX_SHADER);
INCBIN_EXTERN(DEPTH_PIXEL_SHADER);

namespace IWXMVM::GFX
{
    void GraphicsManager::CreateGraphicsResources()
    {
        IDirect3DDevice9* device = D3D9::GetDevice();
        HRESULT hr = D3D_OK;

        ID3DXBuffer* errorMessageBuffer = nullptr;

        ID3DXBuffer* vertexShaderBuffer = nullptr;
        hr = D3DXCompileShader((LPCSTR)VERTEX_SHADER_data, VERTEX_SHADER_size, nullptr, NULL, "main",
            "vs_3_0",
            NULL,
            &vertexShaderBuffer, &errorMessageBuffer, nullptr);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to compile vertex shader: {}",
                reinterpret_cast<const char*>(errorMessageBuffer->GetBufferPointer()));
            errorMessageBuffer->Release();
            return;
        }
        hr = device->CreateVertexShader(reinterpret_cast<const DWORD*>(vertexShaderBuffer->GetBufferPointer()),
            &vertexShader);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create vertex shader");
        }
        vertexShaderBuffer->Release();

        ID3DXBuffer* pixelShaderBuffer = nullptr;
        hr = D3DXCompileShader((LPCSTR)PIXEL_SHADER_data, PIXEL_SHADER_size, nullptr, NULL, "main", "ps_3_0",
            NULL, &pixelShaderBuffer, &errorMessageBuffer, nullptr);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to compile pixel shader: {}",
                reinterpret_cast<const char*>(errorMessageBuffer->GetBufferPointer()));
            errorMessageBuffer->Release();
            return;
        }
        hr = device->CreatePixelShader(reinterpret_cast<const DWORD*>(pixelShaderBuffer->GetBufferPointer()),
            &pixelShader);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create pixel shader");
        }
        pixelShaderBuffer->Release();

        D3DVERTEXELEMENT9 decl[] = {
            {0, offsetof(Types::Vertex, pos), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, offsetof(Types::Vertex, normal), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
            {0, offsetof(Types::Vertex, col), D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END(),
        };
        hr = device->CreateVertexDeclaration(decl, &vertexDeclaration);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create vertex declaration");
        }
    }
    void GraphicsManager::DestroyGraphicsResources()
    {
        if (vertexDeclaration != nullptr)
        {
            vertexDeclaration->Release();
            vertexDeclaration = nullptr;
        }
        if (pixelShader != nullptr)
        {
            pixelShader->Release();
            pixelShader = nullptr;
        }
        if (vertexShader != nullptr)
        {
            vertexShader->Release();
            vertexShader = nullptr;
        }
    }

    void GraphicsManager::CreateDepthPassResources()
    {
        IDirect3DDevice9* device = D3D9::GetDevice();
        HRESULT hr = D3D_OK;

        constexpr Types::FSVertex squareData[] = {
            {{-1.0f, 1.0f, 0.0f}, {0.0f, 0.0f}}, {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, {{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
            {{-1.0f, -1.0f, 0.0f}, {0.0f, 1.0f}}, {{1.0f, -1.0f, 0.0f}, {1.0f, 1.0f}}, {{1.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        };

        hr = device->CreateVertexBuffer(sizeof(squareData), D3DUSAGE_WRITEONLY, NULL, D3DPOOL_DEFAULT, &depthPassVertices, nullptr);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create depth pass vertex buffer");
        }

        void* tmp = nullptr;
        depthPassVertices->Lock(0, sizeof(squareData), &tmp, NULL);
        std::memcpy(tmp, squareData, sizeof(squareData));
        depthPassVertices->Unlock();

        D3DVERTEXELEMENT9 decl[] = {
            {0, offsetof(Types::FSVertex, p), D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
            {0, offsetof(Types::FSVertex, uv), D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
            D3DDECL_END(),
        };
        hr = device->CreateVertexDeclaration(decl, &depthPassVDecl);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create depth pass vertex declaration");
        }

        ID3DXBuffer* errorMessageBuffer = nullptr;

        ID3DXBuffer* vertexShaderBuffer = nullptr;
        hr = D3DXCompileShader((LPCSTR)DEPTH_VERTEX_SHADER_data, DEPTH_VERTEX_SHADER_size, nullptr, NULL, "main",
            "vs_3_0",
            NULL,
            &vertexShaderBuffer, &errorMessageBuffer, nullptr);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to compile depth vertex shader: {}",
                reinterpret_cast<const char*>(errorMessageBuffer->GetBufferPointer()));
            errorMessageBuffer->Release();
            return;
        }
        hr = device->CreateVertexShader(reinterpret_cast<const DWORD*>(vertexShaderBuffer->GetBufferPointer()),
            &depthPassVS);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create depth pass vertex shader");
        }
        vertexShaderBuffer->Release();

        ID3DXBuffer* pixelShaderBuffer = nullptr;
        hr = D3DXCompileShader((LPCSTR)DEPTH_PIXEL_SHADER_data, DEPTH_PIXEL_SHADER_size, nullptr, NULL, "main", "ps_3_0",
            NULL, &pixelShaderBuffer, &errorMessageBuffer, nullptr);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to compile depth pixel shader: {}",
                reinterpret_cast<const char*>(errorMessageBuffer->GetBufferPointer()));
            errorMessageBuffer->Release();
            return;
        }
        hr = device->CreatePixelShader(reinterpret_cast<const DWORD*>(pixelShaderBuffer->GetBufferPointer()),
            &depthPassPS);
        if (FAILED(hr))
        {
            LOG_ERROR("Failed to create depth pass pixel shader");
        }
        pixelShaderBuffer->Release();
    }
    void GraphicsManager::DestroyDepthPassResources()
    {
        depthPassPS->Release();
        depthPassVS->Release();
        depthPassVDecl->Release();
        depthPassVertices->Release();
    }

    void GraphicsManager::Initialize()
    {
        CreateGraphicsResources();
        CreateDepthPassResources();

        BufferManager::Get().Initialize();

        // (the gizmos' meshes are what the mouse grabs; a dynamic mesh takes its place after these)
        BufferManager::Get().AddMesh(&gizmo_translate_x);
        BufferManager::Get().AddMesh(&gizmo_translate_y);
        BufferManager::Get().AddMesh(&gizmo_translate_z);

        BufferManager::Get().AddMesh(&gizmo_rotate_x);
        BufferManager::Get().AddMesh(&gizmo_rotate_y);
        BufferManager::Get().AddMesh(&gizmo_rotate_z);
    }

    void GraphicsManager::Uninitialize()
    {
        BufferManager::Get().Uninitialize();
        DestroyDepthPassResources();
        DestroyGraphicsResources();
    }

    // Where the frame on the screen was drawn from. The game draws a frame from a copy of the camera taken a few frames
    // back, so by now the camera has moved on: turning at 720 degrees a second, 13 degrees on average, which put the lines
    // 155 px off the world. What is drawn over the world and what the mouse picks in it go by
    // this, and it is the camera's for a game that can't tell.
    Types::DrawnView GetFrameView()
    {
        if (const auto drawn = Mod::GetGameInterface()->GetDrawnView())
            return *drawn;

        const auto& camera = Components::CameraManager::Get().GetActiveCamera();
        const auto& rotation = camera->GetRotation();
        const auto rotationMatrix =
            glm::eulerAngleZYX(glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z));
        const glm::vec3 forwardVector = {
            rotationMatrix[0][0],
            rotationMatrix[0][1],
            rotationMatrix[0][2],
        };
        const glm::vec3 upVector = {
            rotationMatrix[2][0],
            rotationMatrix[2][1],
            rotationMatrix[2][2],
        };
        return {camera->GetPosition(), forwardVector, upVector, camera->GetFov()};
    }

    // what a line needs of the view
    ViewLines::Eye GetEye()
    {
        const auto frame = GetFrameView();
        const auto gameView = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::Component::GameView);
        return ViewLines::MakeEye(frame.position, frame.forward, frame.up, frame.fov, gameView->GetViewportSize().x);
    }

    glm::mat4 GetViewMatrix()
    {
        const auto frame = GetFrameView();

        auto view = glm::lookAtLH(frame.position, frame.position + frame.forward, frame.up);
        view[0][0] *= -1.0f;
        view[1][0] *= -1.0f;
        view[2][0] *= -1.0f;
        view[3][0] *= -1.0f;

        return view;
    }

    glm::mat4 GetProjectionMatrix()
    {
        const auto aspectRatio = ImGui::GetMainViewport()->Size.x / ImGui::GetMainViewport()->Size.y;
        const auto tanHalfFovX = glm::tan(glm::radians(GetFrameView().fov) * 0.5f);
        const auto tanHalfFovY = tanHalfFovX * (1.0f / aspectRatio);
        const auto fovY = glm::atan(tanHalfFovY) * 2.0f;
        const auto znear = Mod::GetGameInterface()->GetDvar("r_znear").value().value->floating_point;

        return glm::perspectiveLH_ZO(fovY, aspectRatio, znear, 100000.0f);
    }

    // The same with the game's depth, for what is drawn to compare with the frame's: with the mod's own, a line 1,000 units
    // away is over 100 units too far. Only for drawing: the game has no far plane, its depth never reaches 1, so a ray
    // can't be worked out back through it.
    glm::mat4 GetDrawProjectionMatrix()
    {
        auto projection = GetProjectionMatrix();
        if (const auto drawn = Mod::GetGameInterface()->GetDrawnView(); drawn && drawn->depthScale != 0.0f)
        {
            projection[2][2] = drawn->depthScale;
            projection[3][2] = drawn->depthOffset;
        }
        return projection;
    }

    float GetViewLineDim()
    {
        return GraphicsManager::Get().GetLineDim();
    }

    GizmoState GetGizmoState()
    {
        const auto& graphics = GraphicsManager::Get();
        const auto mode = graphics.GetGizmoMode();
        return {graphics.GetGizmoTarget(),
                graphics.IsGizmoShown(),
                graphics.GetHoveredActor(),
                graphics.GetHoveredPoint(),
                graphics.GetHoveredLine(),
                graphics.GetSelectedNodeId(),
                mode == GizmoMode::Rotate            ? "rotate"
                : mode == GizmoMode::TranslateGlobal ? "world"
                                                     : "local",
                graphics.IsMouseOverView(),
                graphics.WasObjectHoveredThisFrame()};
    }

    void SetPlacingPathPoints(bool placing)
    {
        auto& graphics = GraphicsManager::Get();
        graphics.SetPlacingPathPoints(placing);
        graphics.FocusActor();
    }

    std::optional<glm::vec2> WorldToScreen(glm::vec3 point)
    {
        const auto clip = GetProjectionMatrix() * GetViewMatrix() * glm::vec4(point, 1.0f);
        if (clip.w <= 0.0f)
            return std::nullopt;
        const auto gameView = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::Component::GameView);
        const auto position = gameView->GetViewportPosition(), size = gameView->GetViewportSize();
        const auto ndc = glm::vec2(clip) / clip.w;
        return glm::vec2(position.x + (ndc.x + 1.0f) / 2.0f * size.x, position.y + (1.0f - ndc.y) / 2.0f * size.y);
    }

    void GraphicsManager::SetupRenderState() const noexcept
    {
        IDirect3DDevice9* device = D3D9::GetDevice();

        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        device->SetRenderState(D3DRS_COLORVERTEX, TRUE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);

        const auto identityMatrix = glm::identity<glm::mat4>();
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&identityMatrix));
        device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&identityMatrix));
        device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&identityMatrix));

        const auto viewProjectionMatrix = GetDrawProjectionMatrix() * GetViewMatrix();

        device->SetVertexDeclaration(vertexDeclaration);
        device->SetVertexShaderConstantF(0, reinterpret_cast<const float*>(&viewProjectionMatrix), 4);
        device->SetVertexShader(vertexShader);


        const auto camera = GetFrameView();
        const auto filmtweaks = Mod::GetGameInterface()->GetFilmtweaks();

        // The lit models (the campath's cameras, the orbit camera's axes) have a light of their own, not the map's
        // sun, whose strength differs from map to map and game to game (14 on T5's Nuketown, which turned them white):
        // white, from above and behind the camera, a little to its right, as strong as makes a face it lights fully
        // show the model's own colours with the shader's ambient 0.45, as the model shows them while hovered
        const glm::vec3 up = {0.0f, 0.0f, 1.0f};
        const auto forward = camera.forward;
        // (never zero, as forward is a unit vector; the part to the right is zero looking straight up or down)
        const auto toLight = glm::normalize(-forward + up * 0.8f + glm::cross(forward, up) * 0.4f);
        const glm::vec4 lightDirection = {toLight, 0.0f};
        const glm::vec4 lightColor = {0.55f, 0.55f, 0.55f, 0.0f};
        const glm::vec4 cameraPosition = glm::vec4(camera.position, 0);

        glm::vec4 filmtweaksParams = {};
        if (filmtweaks.enabled)
        {
            filmtweaksParams = {filmtweaks.brightness, filmtweaks.contrast, filmtweaks.desaturation,
                                filmtweaks.invert ? -1.0f : 1.0f};
        }
        else
        {
            filmtweaksParams = {0.0f, 1.0f, 0.0f, 1.0f};
        }

        device->SetPixelShaderConstantF(12, reinterpret_cast<const float*>(&lightColor), 1);
        device->SetPixelShaderConstantF(13, reinterpret_cast<const float*>(&lightDirection), 1);
        device->SetPixelShaderConstantF(14, reinterpret_cast<const float*>(&cameraPosition), 1);
        device->SetPixelShaderConstantF(15, reinterpret_cast<const float*>(&filmtweaksParams), 1);
        device->SetPixelShader(pixelShader);
    }

    glm::vec3 GetMouseRay(ImVec2 mousePos, glm::mat4 projection, glm::mat4 view)
    {
        ImVec2 viewportTopLeft = 
            UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::Component::GameView)->GetViewportPosition();
        ImVec2 viewportSize =
            UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::Component::GameView)->GetViewportSize();

        // the points under the mouse on the near and far planes, as WorldToScreen projects them
        const glm::vec2 ndc((mousePos.x - viewportTopLeft.x) / viewportSize.x * 2.0f - 1.0f,
                            1.0f - (mousePos.y - viewportTopLeft.y) / viewportSize.y * 2.0f);
        const auto inverse = glm::inverse(projection * view);
        const auto nearPoint = inverse * glm::vec4(ndc, 0.0f, 1.0f);
        const auto farPoint = inverse * glm::vec4(ndc, 1.0f, 1.0f);
        return glm::normalize(glm::vec3(farPoint) / farPoint.w - glm::vec3(nearPoint) / nearPoint.w);
    }

    std::pair<glm::vec3, glm::vec3> GetViewRay(glm::vec2 point)
    {
        return {GetFrameView().position, GetMouseRay(ImVec2(point.x, point.y), GetProjectionMatrix(), GetViewMatrix())};
    }

    bool GraphicsManager::MouseIntersects(ImVec2 mousePos, Mesh& mesh, glm::mat4 model)
    {
        const auto position = GetFrameView().position;
        const auto view = GetViewMatrix();
        const auto projection = GetProjectionMatrix();
        const auto mouseRayDirection = GetMouseRay(mousePos, projection, view);

        for (size_t i = 0; i < mesh.indices.size(); i += 3)
        {
            const auto i1 = mesh.indices[i];
            const auto i2 = mesh.indices[i+1];
            const auto i3 = mesh.indices[i+2];
            const auto a = glm::vec3(model * glm::vec4(mesh.vertices[i1].pos, 1.0f));
            const auto b = glm::vec3(model * glm::vec4(mesh.vertices[i2].pos, 1.0f));
            const auto c = glm::vec3(model * glm::vec4(mesh.vertices[i3].pos, 1.0f));
            
            glm::vec2 baryPosition;
            float distance = 0;
            if (glm::intersectRayTriangle(position, glm::normalize(mouseRayDirection), a, b, c, baryPosition, distance))
            {
                return true;
            }
        }
        return false;
    }

    bool GraphicsManager::MouseIntersectsSphereAt(ImVec2 mousePos, glm::vec3 pos, float radius)
    {
        const auto view = GetViewMatrix();
        const auto projection = GetProjectionMatrix();
        const auto mouseRayDirection = GetMouseRay(mousePos, projection, view);

        float distance;
        return glm::intersectRaySphere(GetFrameView().position, glm::normalize(mouseRayDirection), pos, radius * radius,
                                       distance);
    }
    
    void GraphicsManager::DrawGizmoComponent(Mesh& mesh, glm::mat4 model, int32_t axisIndex)
    {
        if (heldAxis.has_value() && heldAxis.value().axisIndex != axisIndex)
        {
            return;
        }

        // (only from the view itself: not through the panels, the strip or a message over it)
        if (!objectHoveredThisFrame && mouseOverView)
        {
            bool mouseIntersects = MouseIntersects(ImGui::GetIO().MousePos, mesh, model);
            objectHoveredThisFrame |= mouseIntersects;
            if (mouseIntersects && Input::KeyDown(ImGuiKey_MouseLeft))
            {
                heldAxis.emplace(TranslationGizmoData{axisIndex, glm::vec3{}});
            }
        }

        // thin lines where its mesh is, to grab it by
        constexpr float ARROW_LENGTH = 16.1f;
        constexpr float RING_RADIUS = 11.5f;
        constexpr float GIZMO_WEIGHT = 1.25f;
        constexpr int RING_POINTS = 64;
        const auto color = &mesh == &gizmo_translate_x || &mesh == &gizmo_rotate_x   ? D3DCOLOR_COLORVALUE(0.89f, 0.23f, 0.23f, 1)
                           : &mesh == &gizmo_translate_y || &mesh == &gizmo_rotate_y ? D3DCOLOR_COLORVALUE(0.23f, 0.82f, 0.29f, 1)
                                                                                     : D3DCOLOR_COLORVALUE(0.23f, 0.39f, 0.89f, 1);
        const auto at = [&](float x, float z) { return glm::vec3(model * glm::vec4(x, 0.0f, z, 1.0f)); };
        if (&mesh == &gizmo_rotate_x || &mesh == &gizmo_rotate_y || &mesh == &gizmo_rotate_z)
        {
            std::vector<glm::vec3> ring;
            for (int i = 0; i <= RING_POINTS; ++i)
            {
                const auto angle = glm::two_pi<float>() * (i % RING_POINTS) / RING_POINTS;
                ring.push_back(at(std::cos(angle) * RING_RADIUS, std::sin(angle) * RING_RADIUS));
            }
            DrawLines(gizmoLines, {ring}, color, GIZMO_WEIGHT);
            return;
        }
        const auto from = at(0.0f, 0.0f), tip = at(ARROW_LENGTH, 0.0f);
        if (const auto head = ViewLines::GetArrowHead(GetEye(), from, tip))
            DrawLines(gizmoLines, {{from, ((*head)[0] + (*head)[1]) * 0.5f}}, color, GIZMO_WEIGHT, LineDepth::Over,
                      std::span(&*head, 1));
    }

    
    std::optional<glm::vec3> GetMousePositionOnPlane(glm::vec3 origin, glm::vec3 normal)
    {
        const auto position = GetFrameView().position;
        const auto view = GetViewMatrix();
        const auto projection = GetProjectionMatrix();
        const auto mouseRayDirection = GetMouseRay(ImGui::GetIO().MousePos, projection, view);

        float intersectionDistance = 0;
        if (glm::intersectRayPlane(position, glm::normalize(mouseRayDirection), origin, glm::normalize(normal),
                                   intersectionDistance))
        {
            return position + intersectionDistance * mouseRayDirection;
        }

        return std::nullopt;
    }

    void GraphicsManager::DrawTranslationGizmo(glm::vec3& position, glm::mat4 translation,
                                               glm::mat4 rotation, float scale)
    {
        auto scaledModel = translation * rotation * glm::scale(glm::vec3(1, 1, 1) * scale);

        if (Input::KeyUp(ImGuiKey_MouseLeft))
        {
            heldAxis = std::nullopt;
        }

        const bool isHeld = heldAxis.has_value();
        DrawGizmoComponent(gizmo_translate_x, scaledModel, 0);

        auto rotatedY = glm::rotate(scaledModel, glm::radians(90.0f), glm::vec3(0, 0, 1));
        DrawGizmoComponent(gizmo_translate_y, rotatedY, 1);

        auto rotatedZ = glm::rotate(scaledModel, glm::radians(-90.0f), glm::vec3(0, 1, 0));
        DrawGizmoComponent(gizmo_translate_z, rotatedZ, 2);

        if (heldAxis.has_value())
        {
            auto axisDirection = glm::vec3(0, 0, 0);
            axisDirection[heldAxis.value().axisIndex] = 1.0f;
            axisDirection = glm::vec3(rotation * glm::vec4(axisDirection, 1.0f));
            axisDirection = glm::normalize(axisDirection);

            auto towardsCamera = glm::normalize(GetFrameView().position - position);

            glm::vec3 bestPlaneNormal = glm::vec3(0);
            float bestDotProduct = 0;
            for (int i = 0; i < 2; i++)
            {
                auto axisNormal = glm::vec3(0, 0, 0);
                axisNormal[(heldAxis.value().axisIndex + i + 1) % 3] = 1.0f;
                axisNormal = glm::vec3(rotation * glm::vec4(axisNormal, 1.0f));
                axisNormal = glm::normalize(axisNormal);

                auto dot = glm::abs(glm::dot(axisNormal, towardsCamera));
                if (dot > bestDotProduct)
                {
                    bestDotProduct = dot;
                    bestPlaneNormal = axisNormal;
                }
            }

            auto intersection = GetMousePositionOnPlane(position, bestPlaneNormal);
            if (intersection.has_value())
            {
                if (!isHeld) 
                {
                    // store offset to account for the distance between object position and mouse position,
                    // so that the object moves relative to the mouse cursor
                    heldAxis.value().objectOffset = intersection.value() - position;
                }

                intersection.value() -= heldAxis.value().objectOffset;
                if (intersection.value() != position)
                {
                    auto dot = glm::dot(glm::normalize(intersection.value() - position), axisDirection);
                    auto distance = glm::distance(intersection.value(), position);
                    position = position + axisDirection * dot * distance;
                }
            }
        }
    }

    void GraphicsManager::DrawRotationGizmo(glm::vec3& rotation, glm::mat4 translation, float scale)
    {
        const auto rotate = glm::eulerAngleZYX(
            glm::radians(rotation.y), glm::radians(rotation.x), glm::radians(rotation.z)
        );
        const auto scaledModel = translation * rotate * glm::scale(glm::vec3(1, 1, 1) * scale);

        if (Input::KeyUp(ImGuiKey_MouseLeft))
        {
            heldAxis = std::nullopt;
        }

        auto rotatedX = glm::rotate(scaledModel, glm::radians(-90.0f), glm::vec3(0, 0, 1));
        DrawGizmoComponent(gizmo_rotate_x, rotatedX, 0);

        DrawGizmoComponent(gizmo_rotate_y, scaledModel, 2);

        auto rotatedZ = glm::rotate(scaledModel, glm::radians(90.0f), glm::vec3(1, 0, 0));
        DrawGizmoComponent(gizmo_rotate_z, rotatedZ, 1);

        if (heldAxis.has_value())
        {
            auto amountX = ImGui::GetIO().MouseDelta.x * 0.008f;
            auto amountY = ImGui::GetIO().MouseDelta.y * 0.008f;

            auto quat = glm::toQuat(rotate);

            auto selectedAxis = heldAxis.value().axisIndex;
            selectedAxis = heldAxis.value().axisIndex == 1 ? 2 : selectedAxis;
            selectedAxis = heldAxis.value().axisIndex == 2 ? 1 : selectedAxis;
            auto axis = glm::vec3(0, 0, 0);
            axis[selectedAxis] = 1;

            quat = quat * glm::angleAxis(amountX + amountY, axis);

            auto newRotation = glm::degrees(glm::eulerAngles(quat));
            rotation[0] = newRotation[1];
            rotation[1] = newRotation[2];
            rotation[2] = newRotation[0];
        }
    }

    // A line through points, a strip that faces the view and has one width on the screen
    void AppendLine(Mesh& mesh, const ViewLines::Eye& eye, const std::vector<glm::vec3>& points, D3DCOLOR color,
                    float widthPixels)
    {
        for (const auto& strip : ViewLines::GetStrips(eye, points, widthPixels))
        {
            for (std::size_t i = 0; i < strip.left.size(); ++i)
            {
                const auto first = static_cast<Types::Index>(mesh.vertices.size());
                mesh.vertices.push_back({.pos = strip.left[i], .normal = glm::vec3(0, 0, 1), .col = color});
                mesh.vertices.push_back({.pos = strip.right[i], .normal = glm::vec3(0, 0, 1), .col = color});
                if (i == 0)
                    continue;
                for (const auto index : {0, 1, 2, 1, 3, 2})
                    mesh.indices.push_back(static_cast<Types::Index>(first - 2 + index));
            }
        }
    }

    D3DCOLOR WithAlpha(D3DCOLOR color, float alpha)
    {
        const auto was = static_cast<float>(color >> 24) / 255.0f;
        return (color & 0x00FFFFFF) | (static_cast<D3DCOLOR>(std::clamp(was * alpha, 0.0f, 1.0f) * 255.0f + 0.5f) << 24);
    }

    bool IsMouseOverGameView();

    // The way the selected actor moves: its path and points, or its keys and the line between them. Drawn before the
    // campath, as the line binds the dynamic buffers.
    void GraphicsManager::DrawActorMovement(const Types::Actor& actor)
    {
        constexpr auto PATH_COLOR = D3DCOLOR_COLORVALUE(0.3f, 0.85f, 1.0f, 1);
        constexpr auto LIFT = glm::vec3(0, 0, 2);
        constexpr float LIT_WEIGHT = 1.5f;
        constexpr float DASH_WEIGHT = 0.5f;
        constexpr float THIN_WEIGHT = 0.75f;
        constexpr float MARKER_PIXELS = 6.0f;
        const auto toColor = [](ImVec4 color) { return D3DCOLOR_COLORVALUE(color.x, color.y, color.z, 1.0f); };
        std::vector<glm::vec3> points, markers;
        if (actor.movement == Types::ActorMovement::Path)
        {
            // as it walks it, round its corners
            for (const auto& point : Components::Actors::GetPathLine(actor))
                points.push_back(point + LIFT);
            markers = actor.path;
        }
        else if (actor.movement == Types::ActorMovement::Keyframes && !actor.keys.empty())
        {
            const auto first = actor.keys.front().tick, last = actor.keys.back().tick;
            const auto steps = std::clamp<std::uint32_t>((last - first) / 50, 1, 200);
            for (std::uint32_t i = 0; i <= steps; ++i)
                points.push_back(
                    Components::Actors::EvaluatePlacement(actor, first + (last - first) * i / steps).position + LIFT);
            for (const auto& key : actor.keys)
                markers.push_back(key.position);
        }

        if (points.size() >= 2 && actor.movement == Types::ActorMovement::Path && !actor.stretches.empty())
        {
            // each stretch in its gait's colour, the one the timeline points at lit:
            // a point of the line is of the stretch whose straight line it is nearest
            const auto stretchOf = [&](glm::vec3 point) {
                std::size_t nearest = 0;
                float best = FLT_MAX;
                for (std::size_t i = 0; i + 1 < actor.path.size(); ++i)
                {
                    const auto a = glm::vec2(actor.path[i]), b = glm::vec2(actor.path[i + 1]);
                    const auto ab = b - a;
                    const auto along =
                        glm::dot(ab, ab) > 0.0f
                            ? std::clamp(glm::dot(glm::vec2(point) - a, ab) / glm::dot(ab, ab), 0.0f, 1.0f)
                            : 0.0f;
                    if (const auto distance = glm::distance(glm::vec2(point), a + ab * along); distance < best)
                    {
                        best = distance;
                        nearest = i;
                    }
                }
                return nearest;
            };
            // (after a death that cuts the walk, the rest isn't reached: SIZE_MAX, dashed grey)
            std::optional<float> cutAt;
            if (const auto cut = Components::Actors::GetCutAt(actor))
                cutAt = ActorLogic::GetPathProgress(actor, *cut, Components::Actors::GetAnimationLookup());
            std::vector<std::pair<std::size_t, std::vector<glm::vec3>>> lines;
            float along = 0.0f;
            for (std::size_t k = 0; k < points.size(); ++k)
            {
                const auto& point = points[k];
                if (k > 0)
                    along += glm::distance(glm::vec2(points[k - 1]), glm::vec2(point));
                const auto stretch = cutAt && along > *cutAt ? SIZE_MAX : stretchOf(point - LIFT);
                if (lines.empty() || lines.back().first != stretch)
                {
                    lines.push_back({stretch, {}});
                    if (lines.size() > 1)
                        lines.back().second.push_back(lines[lines.size() - 2].second.back());
                }
                lines.back().second.push_back(point);
            }
            // a line in dashes 8 units long
            const auto dashed = [](const std::vector<glm::vec3>& line) {
                std::vector<std::vector<glm::vec3>> dashes;
                float along = 0.0f;
                for (std::size_t k = 0; k + 1 < line.size(); ++k)
                {
                    const auto a = line[k], b = line[k + 1];
                    const auto length = glm::distance(a, b);
                    for (float t = 0.0f; t < length;)
                    {
                        const auto phase = std::fmod(along + t, 16.0f);
                        const auto step = std::min(length - t, phase < 8.0f ? 8.0f - phase : 16.0f - phase);
                        if (phase < 8.0f)
                            dashes.push_back({a + (b - a) * (t / length), a + (b - a) * ((t + step) / length)});
                        t += std::max(step, 0.01f);
                    }
                    along += length;
                }
                return dashes;
            };
            for (const auto& [stretch, line] : lines)
            {
                if (stretch == SIZE_MAX)
                {
                    DrawLines(actorPath, dashed(line), D3DCOLOR_COLORVALUE(0.6f, 0.6f, 0.6f, 0.7f), THIN_WEIGHT,
                              LineDepth::Faint);
                    continue;
                }
                const auto* gait = ActorLogic::GetStretch(actor, stretch);
                const bool lit = highlightedStretch && highlightedStretch->first == actor.id &&
                                 highlightedStretch->second == stretch;
                DrawLines(actorPath, {line},
                          lit ? D3DCOLOR_COLORVALUE(1, 1, 1, 1) : toColor(UI::GetGaitColor(gait->gait, gait->crouched)),
                          lit ? LIT_WEIGHT : 1.0f, LineDepth::Faint);
                // a stretch with a look of its own: dashed white over it
                if (gait && !gait->look.empty() && !lit)
                    DrawLines(actorPath, dashed(line), D3DCOLOR_COLORVALUE(1, 1, 1, 0.9f), DASH_WEIGHT, LineDepth::Faint);
            }
            // a ring at each point it waits at
            for (std::size_t i = 0; i + 1 < actor.path.size(); ++i)
                if (const auto* stretch = ActorLogic::GetStretch(actor, i); stretch && stretch->wait > 0)
                {
                    std::vector<glm::vec3> ring;
                    for (int k = 0; k <= 24; ++k)
                    {
                        const auto angle = glm::two_pi<float>() * k / 24;
                        ring.push_back(actor.path[i] + LIFT + glm::vec3(std::cos(angle), std::sin(angle), 0) * 12.0f);
                    }
                    DrawLines(actorPath, {ring}, toColor(UI::WAIT_COLOR));
                }
        }
        else if (points.size() >= 2)
            DrawLines(actorPath, {points}, PATH_COLOR, 1.0f, LineDepth::Faint);

        // its points, in the colour of the stretch that sets off from them, the selected one's filled
        const auto selectedPoint = Components::Actors::GetSelectedWaypoint();
        for (std::size_t i = 0; i < markers.size(); ++i)
        {
            const auto* gait = actor.movement == Types::ActorMovement::Path && !actor.stretches.empty()
                                   ? ActorLogic::GetStretch(actor, std::min(i, actor.path.size() - 2))
                                   : nullptr;
            DrawPoint(markers[i] + LIFT, gait ? toColor(UI::GetGaitColor(gait->gait, gait->crouched)) : PATH_COLOR,
                      actor.movement == Types::ActorMovement::Path && selectedPoint == i, ViewLines::POINT_PIXELS);
        }

        // the point under the mouse, and where the actor is at the tick the timeline points at
        if (hoveredPoint && *hoveredPoint < actor.path.size())
        {
            std::vector<glm::vec3> ring;
            for (int k = 0; k <= 24; ++k)
            {
                const auto angle = glm::two_pi<float>() * k / 24;
                ring.push_back(actor.path[*hoveredPoint] + LIFT +
                               glm::vec3(std::cos(angle), std::sin(angle), 0) * 9.0f);
            }
            DrawLines(actorPath, {ring}, D3DCOLOR_COLORVALUE(1, 1, 1, 1));
        }
        if (highlightedStretch && highlightedStretch->first == actor.id && highlightedTick)
        {
            const auto at = Components::Actors::EvaluatePlacement(actor, *highlightedTick).position + LIFT;
            DrawPoint(at, D3DCOLOR_COLORVALUE(1, 1, 1, 1), true, MARKER_PIXELS);
        }
        // the transitions just planned again: their corners, for a moment
        for (const auto& flash : Components::Actors::GetFlashes())
        {
            if (flash.actor != actor.id || std::chrono::steady_clock::now() - flash.at > std::chrono::milliseconds(400))
                continue;
            for (const auto id : flash.clips)
            {
                const auto clip = std::ranges::find(actor.clips, id, &Types::ActorClip::id);
                if (clip == actor.clips.end())
                    continue;
                const auto at = clip->corner && *clip->corner < actor.path.size() ? actor.path[*clip->corner]
                                : clip->chain == Types::ActorChain::Stop && !actor.path.empty()  ? actor.path.back()
                                : clip->chain == Types::ActorChain::Start && !actor.path.empty() ? actor.path.front()
                                                                                                 : glm::vec3(FLT_MAX);
                if (at.x == FLT_MAX)
                    continue;
                std::vector<glm::vec3> ring;
                for (int k = 0; k <= 24; ++k)
                {
                    const auto angle = glm::two_pi<float>() * k / 24;
                    ring.push_back(at + LIFT + glm::vec3(std::cos(angle), std::sin(angle), 0) * 20.0f);
                }
                DrawLines(actorPath, {ring}, D3DCOLOR_COLORVALUE(1, 1, 1, 1));
            }
        }
    }

    // The paths of the actors that aren't selected, thin and dim in their gaits' colours, so they can be found and
    // picked in the view (View > Other actors' paths)
    void GraphicsManager::DrawOtherPaths(std::optional<std::uint32_t> selected)
    {
        constexpr auto LIFT = glm::vec3(0, 0, 2);
        constexpr float DIM = 0.55f;
        constexpr float WEIGHT = 0.75f;
        const auto toColor = [](ImVec4 color) {
            return D3DCOLOR_COLORVALUE(color.x * DIM, color.y * DIM, color.z * DIM, 1.0f);
        };
        for (const auto& actor : Components::Actors::GetActors())
        {
            if (actor.id == selected || actor.hidden || actor.movement != Types::ActorMovement::Path ||
                actor.path.size() < 2)
                continue;
            std::vector<glm::vec3> line;
            for (const auto& point : Components::Actors::GetPathLine(actor))
                line.push_back(point + LIFT);
            const auto* stretch = actor.stretches.empty() ? nullptr : ActorLogic::GetStretch(actor, 0);
            const auto color =
                toColor(stretch ? UI::GetGaitColor(stretch->gait, stretch->crouched) : ImVec4(0.3f, 0.85f, 1.0f, 1.0f));
            DrawLines(actorPath, {line}, color, WEIGHT, LineDepth::Faint);
            for (const auto& point : actor.path)
                DrawPoint(point + LIFT, color, false, ViewLines::OTHER_POINT_PIXELS);
        }
    }

    // A point of another actor's path under the mouse: a click selects that actor and the point
    void GraphicsManager::PickOtherPathPoint(std::optional<std::uint32_t> selected)
    {
        if (hoveredPoint || hoveredLine || !IsMouseOverGameView() || heldAxis.has_value() || placingPathPoints)
            return;
        constexpr auto LIFT = glm::vec3(0, 0, 2);
        const auto mouse = glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        std::optional<std::pair<std::uint32_t, std::size_t>> found;
        float best = 8.0f;
        for (const auto& actor : Components::Actors::GetActors())
        {
            if (actor.id == selected || actor.hidden || actor.movement != Types::ActorMovement::Path)
                continue;
            for (std::size_t i = 0; i < actor.path.size(); ++i)
                if (const auto screen = GFX::WorldToScreen(actor.path[i] + LIFT);
                    screen && glm::distance(*screen, mouse) < best)
                {
                    best = glm::distance(*screen, mouse);
                    found = {actor.id, i};
                }
        }
        if (!found)
            return;
        objectHoveredThisFrame = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (Input::KeyDown(ImGuiKey_MouseLeft))
        {
            Components::Actors::Select(found->first);
            Components::Actors::SelectWaypoint(found->second);
            gizmoTarget = GizmoTarget::Actor;
            UI::UIManager::Get().ShowPicked(UI::PanelLogic::Kind::Actor);
        }
    }

    // The selected actor's path point under the mouse (within a few pixels of where it is drawn), else the line from
    // one: a click selects the point (its gizmo moves it), a right-click opens the view's menu for it (see GameView)
    void GraphicsManager::PickPathPoint(const Types::Actor& actor)
    {
        hoveredPoint.reset();
        hoveredLine.reset();
        if (actor.movement != Types::ActorMovement::Path || !IsMouseOverGameView() || heldAxis.has_value() ||
            placingPathPoints)
            return;
        constexpr auto LIFT = glm::vec3(0, 0, 2);
        const auto mouse = glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y);
        std::vector<std::optional<glm::vec2>> screen;
        for (const auto& point : actor.path)
            screen.push_back(GFX::WorldToScreen(point + LIFT));
        float best = 10.0f;
        for (std::size_t i = 0; i < screen.size(); ++i)
            if (screen[i] && glm::distance(*screen[i], mouse) < best)
            {
                best = glm::distance(*screen[i], mouse);
                hoveredPoint = i;
            }
        if (!hoveredPoint)
        {
            best = 6.0f;
            for (std::size_t i = 0; i + 1 < screen.size(); ++i)
            {
                if (!screen[i] || !screen[i + 1])
                    continue;
                const auto ab = *screen[i + 1] - *screen[i];
                const auto along = glm::dot(ab, ab) > 0.0f
                                       ? std::clamp(glm::dot(mouse - *screen[i], ab) / glm::dot(ab, ab), 0.0f, 1.0f)
                                       : 0.0f;
                if (const auto distance = glm::distance(mouse, *screen[i] + ab * along); distance < best)
                {
                    best = distance;
                    hoveredLine = i;
                }
            }
        }
        if (!hoveredPoint && !hoveredLine)
            return;
        // (the click selects no actor and doesn't put the gizmo away)
        objectHoveredThisFrame = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (hoveredPoint && Input::KeyDown(ImGuiKey_MouseLeft))
        {
            Components::Actors::SelectWaypoint(*hoveredPoint);
            gizmoTarget = GizmoTarget::Actor;
            UI::UIManager::Get().ShowPicked(UI::PanelLogic::Kind::Actor);
        }
    }

    // Lines as AppendLine makes them, from the dynamic buffers; binds the static ones again after. See through: blended,
    // writing no depth, and with their faint part drawn first, past the depth test, under the full one.
    void GraphicsManager::DrawLines(Mesh& mesh, const std::vector<std::vector<glm::vec3>>& lines, D3DCOLOR color,
                                    float weight, LineDepth depth, std::span<const std::array<glm::vec3, 3>> fills)
    {
        // (lines lie on the ground: this keeps them in front of it, a tenth of a unit at 200 units)
        constexpr float DEPTH_BIAS = -0.00001f;
        constexpr DWORD NO_DEPTH_BIAS = 0;
        const auto& preferences = PreferencesConfiguration::Get();
        const auto width = preferences.viewLineWidth * weight;
        const auto strength = preferences.viewLineStrength * lineDim;
        const auto eye = GetEye();
        const auto draw = [&](D3DCOLOR passColor, float widthPixels, bool withFills) {
            mesh.vertices.clear();
            mesh.indices.clear();
            for (const auto& line : lines)
                AppendLine(mesh, eye, line, passColor, widthPixels);
            for (std::size_t i = 0; withFills && i < fills.size(); ++i)
            {
                const auto first = static_cast<Types::Index>(mesh.vertices.size());
                for (const auto& corner : fills[i])
                    mesh.vertices.push_back({.pos = corner, .normal = glm::vec3(0, 0, 1), .col = passColor});
                for (const auto index : {0, 1, 2})
                    mesh.indices.push_back(static_cast<Types::Index>(first + index));
            }
            if (mesh.indices.empty())
                return;
            // (after the frame's other lines: see Render)
            BufferManager::Get().AddMesh(&mesh, BufferType::Dynamic);
            BufferManager::Get().BindBuffers(BufferType::Dynamic);
            BufferManager::Get().DrawMesh(mesh, glm::identity<glm::mat4>(), true);
        };

        IDirect3DDevice9* device = D3D9::GetDevice();
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        if (depth == LineDepth::Faint)
        {
            draw(WithAlpha(color, strength * ViewLines::BEHIND_WALLS), width, true);
            device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
            device->SetRenderState(D3DRS_DEPTHBIAS, std::bit_cast<DWORD>(DEPTH_BIAS));
        }
        draw(WithAlpha(D3DCOLOR_COLORVALUE(0, 0, 0, 1), strength * ViewLines::EDGE_ALPHA), width + ViewLines::EDGE_PIXELS,
             false);
        draw(WithAlpha(color, strength), width, true);
        device->SetRenderState(D3DRS_DEPTHBIAS, NO_DEPTH_BIAS);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        device->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        BufferManager::Get().BindBuffers(BufferType::Static);
    }

    void GraphicsManager::DrawPoint(glm::vec3 point, D3DCOLOR color, bool filled, float radiusPixels)
    {
        const auto ring = ViewLines::GetRing(GetEye(), point, radiusPixels);
        if (ring.empty())
            return;
        std::vector<std::array<glm::vec3, 3>> fills;
        for (std::size_t i = 0; filled && i + 1 < ring.size(); ++i)
            fills.push_back({point, ring[i], ring[i + 1]});
        DrawLines(actorPath, {ring}, color, 1.0f, LineDepth::Faint, fills);
    }

    void GraphicsManager::DrawAxes(glm::vec3 position, const glm::mat3& axes, float length)
    {
        const std::array colors{D3DCOLOR_COLORVALUE(0.89f, 0.23f, 0.23f, 1), D3DCOLOR_COLORVALUE(0.23f, 0.82f, 0.29f, 1),
                                D3DCOLOR_COLORVALUE(0.23f, 0.39f, 0.89f, 1)};
        for (int i = 0; i < 3; ++i)
            DrawLines(gizmoLines, {{position - axes[i] * length, position + axes[i] * length}}, colors[i]);
    }

    // whether the mouse is over the game's picture, and not over a window or popup on top of it
    bool IsMouseOverGameView()
    {
        const auto* hovered = ImGui::GetCurrentContext()->HoveredWindow;
        if (!hovered || std::string_view(hovered->RootWindow->Name) != "GameView")
            return false;
        const auto gameView = UI::UIManager::Get().GetUIComponent<UI::GameView>(UI::Component::Component::GameView);
        const auto position = gameView->GetViewportPosition(), size = gameView->GetViewportSize();
        const auto mouse = ImGui::GetIO().MousePos;
        // (the strip and the messages along the bottom of the view take their own clicks)
        return mouse.x >= position.x && mouse.y >= position.y && mouse.x < position.x + size.x &&
               mouse.y < position.y + size.y && !gameView->IsMouseOverOverlay();
    }

    // The visible actor nearest the camera under the mouse, if any
    namespace
    {
        constexpr float LIGHT_PICK_RADIUS = 14.0f;
        // a key's dot on a light's path, in the timeline's colour of keys
        constexpr auto KEY_DOT_COLOR = D3DCOLOR_COLORVALUE(0.88f, 0.69f, 0.31f, 1.0f);

        // Where a light is: on the bone it follows, or where it was put, as its keys have it at the playhead
        Components::LightLogic::Placed GetLightPlace(const Types::Actor& placedActor)
        {
            const auto actor =
                Components::Lights::WithKeyedPlacement(placedActor, Components::Playback::GetTimelineTick());
            if (const auto& follow = actor.light->follow)
                if (const auto bone = Mod::GetGameInterface()->GetTargetBone(follow->target, follow->bone))
                    return Components::LightLogic::Place(actor, bone->position, bone->axes);
            return Components::LightLogic::Place(actor);
        }
    }  // namespace

    std::optional<std::uint32_t> GraphicsManager::PickActor(std::uint32_t tick) const
    {
        if (!IsMouseOverGameView() || heldAxis.has_value())
            return std::nullopt;
        const auto camera = GetFrameView().position;
        const auto ray = glm::normalize(GetMouseRay(ImGui::GetIO().MousePos, GetProjectionMatrix(), GetViewMatrix()));
        std::optional<std::uint32_t> picked;
        float nearest = FLT_MAX;
        for (const auto& actor : Components::Actors::GetActors())
        {
            if (!Components::Actors::IsVisible(actor, tick))
                continue;
            const auto feet = Components::Actors::EvaluatePlacement(actor, tick).position;
            // (a killstreak by its body: a helicopter's origin is its rotor)
            const auto killstreak = Components::KillstreakLogic::FindByModels(actor.models);
            const auto sphere =
                killstreak ? Components::KillstreakLogic::GetPickSphere(Components::KillstreakLogic::Get(*killstreak))
                           : Components::KillstreakLogic::PickSphere{};
            // (a light by its icon: not one the camera is in, as a spot just added is, which every click would hit)
            const auto light = actor.light ? GetLightPlace(actor).position : feet;
            if (actor.light && glm::distance(camera, light) <= LIGHT_PICK_RADIUS)
                continue;
            const auto hit = actor.light ? ActorLogic::RayHitsSphere(camera, ray, light, LIGHT_PICK_RADIUS)
                             : killstreak ? ActorLogic::RayHitsSphere(
                                                camera, ray, feet + glm::vec3(0.0f, 0.0f, sphere.up), sphere.radius)
                                          : ActorLogic::RayHitsActor(camera, ray, feet);
            if (hit && *hit < nearest)
            {
                nearest = *hit;
                picked = actor.id;
            }
        }
        return picked;
    }

    // Each light's icon in its own colour, the selected one's reach with it, and
    // its path through its keys (3.13). Not while recording (see Render).
    void GraphicsManager::DrawLights(std::optional<std::uint32_t> selected)
    {
        namespace LightLogic = Components::LightLogic;
        const auto actors = Components::Actors::GetActors();
        if (std::ranges::none_of(actors, [](const Types::Actor& actor) { return actor.light.has_value(); }))
            return;
        const auto camera = GetFrameView();
        const auto forward = glm::normalize(camera.forward);
        const auto right = glm::normalize(glm::cross(forward, glm::vec3(0.0f, 0.0f, 1.0f)));
        const auto up = glm::cross(right, forward);

        for (const auto& actor : actors)
        {
            if (!actor.light || actor.hidden)
                continue;
            const auto placed = GetLightPlace(actor);
            const auto size = std::clamp(glm::length(placed.position - camera.position) * 0.02f, 4.0f, 30.0f);
            const auto color = glm::clamp(actor.light->color + 0.25f, 0.0f, 1.0f);
            DrawLines(lightLines, LightLogic::GetIconLines(placed.position, right, up, size),
                      D3DCOLOR_COLORVALUE(color.r, color.g, color.b, 1));
            if (actor.id != selected)
                continue;
            DrawLines(lightLines,
                      LightLogic::GetReachLines(actor.light->kind, placed, actor.light->radius, actor.light->cone),
                      D3DCOLOR_COLORVALUE(1, 1, 1, 0.75f), 0.75f);
            // its path through its position's keys, a dot on each, as the campath's line (3.13); on a bone
            // it goes with the bone
            if (actor.light->follow)
                continue;
            const auto& keys = Components::KeyframeManager::Get().GetKeyframes(
                Components::Lights::GetProperty(actor.light->id, LightLogic::Channel::Position));
            if (keys.size() < 2)
                continue;
            DrawLines(lightLines, {LightLogic::GetPathLine(keys)}, D3DCOLOR_COLORVALUE(1, 1, 1, 0.8f), 1.0f,
                      LineDepth::Faint);
            for (const auto& key : keys)
                DrawPoint(key.value.vector3, KEY_DOT_COLOR, true, 5.0f);
        }
    }

    // The selected prop's path through its position's keys, a dot on each, as a light's.
    // Not while recording (see Render).
    void GraphicsManager::DrawPropPath(std::optional<std::uint32_t> selected)
    {
        const auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        if (!actor || !actor->placedProp || actor->hidden)
            return;
        const auto& keys = Components::KeyframeManager::Get().GetKeyframes(
            Components::Props::GetProperty(actor->placedProp->id, Components::PropLogic::Channel::Position));
        if (keys.size() < 2)
            return;
        DrawLines(lightLines, {Components::LightLogic::GetPathLine(keys)}, D3DCOLOR_COLORVALUE(1, 1, 1, 0.8f), 1.0f,
                  LineDepth::Faint);
        for (const auto& key : keys)
            DrawPoint(key.value.vector3, KEY_DOT_COLOR, true, 5.0f);
    }

    // The selected Mortar Team's points: a ring as wide as a shell's blast about
    // each, with a line up the way its shells come down. Not while recording (see Render).
    void GraphicsManager::DrawMortarPoints(std::optional<std::uint32_t> selected)
    {
        constexpr float BLAST = 256.0f;  // _mortar.gsc's losRadiusDamage
        constexpr float COMES = 300.0f;
        constexpr int SIDES = 48;
        const auto actor = selected ? Components::Actors::GetActor(*selected) : std::nullopt;
        if (!actor || !actor->mortar || actor->hidden)
            return;
        const auto radians = glm::radians(actor->yaw);
        const glm::vec3 ahead(std::cos(radians), std::sin(radians), 0.0f);
        const auto down = glm::radians(75.0f);
        std::vector<std::vector<glm::vec3>> lines;
        for (const auto& point : actor->mortar->points)
        {
            std::vector<glm::vec3> ring;
            for (int i = 0; i <= SIDES; ++i)
            {
                const auto angle = glm::two_pi<float>() * static_cast<float>(i) / SIDES;
                ring.push_back(point + glm::vec3(std::cos(angle) * BLAST, std::sin(angle) * BLAST, 2.0f));
            }
            lines.push_back(std::move(ring));
            lines.push_back(
                {point, point - ahead * (COMES * std::cos(down)) + glm::vec3(0.0f, 0.0f, COMES * std::sin(down))});
        }
        DrawLines(lightLines, lines, D3DCOLOR_COLORVALUE(1.0f, 0.55f, 0.3f, 0.9f), 1.0f, LineDepth::Faint);
        for (const auto& point : actor->mortar->points)
            DrawPoint(point, KEY_DOT_COLOR, true, 6.0f);
    }

    // A ring around the feet of the selected actor and of the one under the mouse
    void GraphicsManager::DrawActorRings(std::optional<std::uint32_t> selected, std::uint32_t tick)
    {
        constexpr auto SELECTED_COLOR = D3DCOLOR_COLORVALUE(0.3f, 0.85f, 1.0f, 1);
        constexpr auto HOVERED_COLOR = D3DCOLOR_COLORVALUE(1.0f, 1.0f, 1.0f, 1);
        auto ring = [&](std::uint32_t id) {
            std::vector<glm::vec3> points;
            const auto actor = Components::Actors::GetActor(id);
            if (!actor)
                return points;
            const auto feet = Components::Actors::EvaluatePlacement(*actor, tick).position + glm::vec3(0, 0, 2);
            for (int i = 0; i <= 32; ++i)
            {
                const auto angle = glm::two_pi<float>() * i / 32;
                points.push_back(feet + glm::vec3(std::cos(angle), std::sin(angle), 0) * 18.0f);
            }
            return points;
        };

        // (one whose name the mouse is over in the list, thicker)
        const auto lit = listHoveredActor ? listHoveredActor : hoveredActor;
        const auto weight = listHoveredActor ? 2.0f : 1.0f;
        if (lit && lit != selected)
            DrawLines(actorRing, {ring(*lit)}, HOVERED_COLOR, weight);
        if (selected)
            DrawLines(actorRing, {ring(*selected)}, lit == selected ? HOVERED_COLOR : SELECTED_COLOR,
                      lit == selected ? weight : 1.0f);
    }

    // While placing path points: a click on the world (not on a gizmo's axis) adds a point after the selected one, or
    // at the end, where the click hits the ground, which the game finds
    void GraphicsManager::PlacePathPoint(const Types::Actor& actor)
    {
        placingPreview.reset();
        if (!placingPathPoints)
            return;
        // (Enter and Escape stop it too: the placing.finish command)
        if (actor.movement != Types::ActorMovement::Path)
        {
            placingPathPoints = false;
            return;
        }
        if (!IsMouseOverGameView() || heldAxis.has_value())
            return;

        // so the click selects no actor and doesn't put the gizmo away
        objectHoveredThisFrame = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        const auto camera = GetFrameView().position;
        const auto ray = glm::normalize(GetMouseRay(ImGui::GetIO().MousePos, GetProjectionMatrix(), GetViewMatrix()));

        // the stretch to the mouse, on a level with the point it goes from (the game finds the ground once clicked),
        // in the gait's colour, and how long it takes, for the hint by the mouse
        const auto selectedPoint = Components::Actors::GetSelectedWaypoint();
        const auto from = selectedPoint && *selectedPoint < actor.path.size() ? *selectedPoint
                          : actor.path.empty()                                ? std::size_t{0}
                                                                              : actor.path.size() - 1;
        if (from < actor.path.size() && std::abs(ray.z) > 1e-4f)
        {
            const auto origin = camera;
            const auto along = (actor.path[from].z - origin.z) / ray.z;
            if (along > 0.0f)
            {
                const auto ground = origin + ray * along;
                // (the next stretch's gait: picked with 1, 2, 3 (placing.walk...), else as the last stretch walked)
                auto gait = Components::Actors::GetGait(actor, from);
                if (gait && placingGait)
                {
                    for (const auto& preset : Components::Actors::GetGaits(actor))
                        if ((preset.gait == ActorLogic::Gait::Sprint) ==
                                (placingGait->first == Types::ActorGait::Sprint) &&
                            (preset.gait == ActorLogic::Gait::Run) == (placingGait->first == Types::ActorGait::Run) &&
                            (preset.stance == Types::ActorStance::Crouch) == placingGait->second)
                            gait->speed = preset.speed * gait->pace;
                    gait->gait = placingGait->first;
                    gait->crouched = placingGait->second;
                }
                const auto color =
                    gait ? UI::GetGaitColor(gait->gait, gait->crouched) : ImVec4(0.3f, 0.85f, 1.0f, 1.0f);
                // dashed
                std::vector<std::vector<glm::vec3>> dashes;
                const auto length = glm::distance(actor.path[from], ground);
                for (float d = 0.0f; d < length; d += 16.0f)
                    dashes.push_back(
                        {glm::mix(actor.path[from], ground, d / length) + glm::vec3(0, 0, 2),
                         glm::mix(actor.path[from], ground, std::min(d + 9.0f, length) / length) + glm::vec3(0, 0, 2)});
                DrawLines(actorPath, dashes, D3DCOLOR_COLORVALUE(color.x, color.y, color.z, 0.9f));
                placingPreview = PlacingPreview{
                    glm::vec2(ImGui::GetIO().MousePos.x, ImGui::GetIO().MousePos.y),
                    gait && gait->speed > 0.0f ? length / gait->speed : 0.0f,
                    gait ? UI::ActorActions::GetGaitName(actor, gait->gait, gait->crouched) : std::string()};
            }
        }

        // a double-click places the last point (its first click placed it) and stops
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
        {
            placingPathPoints = false;
            placingPreview.reset();
            return;
        }
        if (!Input::KeyDown(ImGuiKey_MouseLeft))
            return;
        // (the stretch to it at the gait picked for it, if one was)
        UI::ActorActions::AddPathPoint(actor.id, camera, ray, placingGait);
    }

    // The size of an actor's gizmo at a point: about the same on screen from any distance, reaching an actor's knee
    // from 200 units (the campath's, 48 units, covers it to the waist), within limits so it stays usable up close and
    // far away
    float GetActorGizmoScale(glm::vec3 point)
    {
        const auto distance = glm::distance(GetFrameView().position, point);
        return 1.5f * std::clamp(distance / 200.0f, 0.5f, 4.0f);
    }

    // The selected actor's gizmo: its path point's, or where it stands now (its position, its key here, or its whole
    // path)
    void GraphicsManager::DrawActorGizmo(Types::Actor& actor, std::uint32_t tick)
    {
        const auto waypoint = Components::Actors::GetSelectedWaypoint();
        if (actor.movement == Types::ActorMovement::Path && waypoint && *waypoint < actor.path.size())
        {
            // a point of the path, along the world's axes
            auto point = actor.path[*waypoint];
            DrawTranslationGizmo(point, glm::translate(point), glm::identity<glm::mat4>(), GetActorGizmoScale(point));
            if (point != actor.path[*waypoint])
                ActorLogic::MoveWaypoint(actor, *waypoint, point);
            return;
        }

        // a light where its keys have it at the playhead, and a drag keys it there, as a change of its row does;
        // on a bone it goes by its Offset
        if (actor.light)
        {
            if (!actor.light->follow)
                DrawLightGizmo(actor);
            return;
        }
        // (a prop the same)
        if (actor.placedProp)
        {
            DrawPropGizmo(actor);
            return;
        }
        auto placement = Components::Actors::EvaluatePlacement(actor, tick);
        const auto scale = GetActorGizmoScale(placement.position);
        const auto translate = glm::translate(placement.position);
        const auto rotate = glm::eulerAngleZYX(glm::radians(placement.yaw), 0.0f, 0.0f);
        const bool point = actor.light && actor.light->kind == Types::LightKind::Point;
        if (gizmoMode == GizmoMode::Rotate && actor.movement != Types::ActorMovement::Path && !point)
        {
            glm::vec3 rotation(0.0f, placement.yaw, 0.0f);
            DrawRotationGizmo(rotation, translate, scale);
            placement.yaw = std::remainder(rotation.y, 360.0f);
        }
        else
        {
            DrawTranslationGizmo(placement.position, translate,
                                 gizmoMode == GizmoMode::TranslateLocal ? rotate : glm::identity<glm::mat4>(), scale);
        }
        const auto before = Components::Actors::EvaluatePlacement(actor, tick);
        if (placement.position != before.position || placement.yaw != before.yaw)
            Components::Actors::ApplyPlacement(actor, tick, placement.position, placement.yaw);
    }

    void GraphicsManager::DrawLightGizmo(Types::Actor& actor)
    {
        const auto keyed = Components::Lights::WithKeyedPlacementNow(actor, Components::Playback::GetTimelineTick());
        auto position = keyed.position;
        auto yaw = keyed.yaw;
        const auto scale = GetActorGizmoScale(position);
        const auto translate = glm::translate(position);
        // (a Point has nothing to turn)
        if (gizmoMode == GizmoMode::Rotate && actor.light->kind == Types::LightKind::Spot)
        {
            glm::vec3 rotation(0.0f, yaw, 0.0f);
            DrawRotationGizmo(rotation, translate, scale);
            yaw = rotation.y;
        }
        else
        {
            const auto rotate = glm::eulerAngleZYX(glm::radians(yaw), 0.0f, 0.0f);
            DrawTranslationGizmo(position, translate,
                                 gizmoMode == GizmoMode::TranslateLocal ? rotate : glm::identity<glm::mat4>(), scale);
        }
        if (position == keyed.position && yaw == keyed.yaw)
            return;
        auto placed = keyed;
        placed.position = position;
        placed.yaw = yaw;
        UI::ActorActions::MoveLight(actor, placed, "move " + actor.name);
    }

    void GraphicsManager::DrawPropGizmo(Types::Actor& actor)
    {
        const auto keyed = Components::Props::WithKeyedPlacementNow(actor, Components::Playback::GetTimelineTick());
        auto position = keyed.position;
        auto yaw = keyed.yaw;
        const auto scale = GetActorGizmoScale(position);
        const auto translate = glm::translate(position);
        if (gizmoMode == GizmoMode::Rotate)
        {
            glm::vec3 rotation(0.0f, yaw, 0.0f);
            DrawRotationGizmo(rotation, translate, scale);
            yaw = rotation.y;
        }
        else
        {
            const auto rotate = glm::eulerAngleZYX(glm::radians(yaw), 0.0f, 0.0f);
            DrawTranslationGizmo(position, translate,
                                 gizmoMode == GizmoMode::TranslateLocal ? rotate : glm::identity<glm::mat4>(), scale);
        }
        if (position == keyed.position && yaw == keyed.yaw)
            return;
        auto placed = keyed;
        placed.position = position;
        placed.yaw = yaw;
        UI::ActorActions::MoveProp(actor, placed, "move " + actor.name);
    }

    // The campath as the camera goes along it, a point every 20 units
    std::vector<glm::vec3> GraphicsManager::GetCampathLine()
    {
        constexpr float SAMPLES_PER_UNIT = 0.05f;

        auto& keyframeManager = Components::KeyframeManager::Get();
        const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
        const auto& nodes = keyframeManager.GetKeyframes(property);

        std::vector<glm::vec3> line;
        for (std::size_t i = 0; i + 1 < nodes.size(); i++)
        {
            const auto distance =
                glm::distance(nodes[i].value.cameraData.position, nodes[i + 1].value.cameraData.position);
            for (float t = 0.0f; t < 1.0f; t += 1.0f / std::max(distance * SAMPLES_PER_UNIT, 1.0f))
            {
                const float tick = nodes[i + 1].tick * t + nodes[i].tick * (1.0f - t);
                line.push_back(keyframeManager.Interpolate(property, tick).cameraData.position);
            }
        }
        if (!nodes.empty())
            line.push_back(nodes.back().value.cameraData.position);
        return line;
    }

    void GraphicsManager::DrawStreamsShader(Components::PassType passType, bool onlyDrawViewmodel,
                                            bool greenscreenBackground) const
    {
        IDirect3DDevice9* device = D3D9::GetDevice();
        IDirect3DTexture9* depthTexture = D3D9::GetDepthTexture();

        if (!depthTexture)
        {
            return;
        }

        IDirect3DStateBlock9* d3d9_state_block = nullptr;
        D3DMATRIX last_world = {}, last_view = {}, last_projection = {};

        // Backup the DX9 state
        HRESULT result = device->CreateStateBlock(D3DSBT_ALL, &d3d9_state_block);
        if (FAILED(result))
        {
            throw std::runtime_error("Failed to get the D3D9 state block");
        }

        device->GetTransform(D3DTS_WORLD, &last_world);
        device->GetTransform(D3DTS_VIEW, &last_view);
        device->GetTransform(D3DTS_PROJECTION, &last_projection);

        device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        device->SetRenderState(D3DRS_CULLMODE, D3DCULL_CW);
        device->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
        device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
        device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        device->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
        device->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
        device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        device->SetRenderState(D3DRS_CLIPPING, FALSE);
        device->SetRenderState(D3DRS_LIGHTING, FALSE);
        if (greenscreenBackground)
        {
            device->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
            device->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
            device->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
        }

        const auto identityMatrix = glm::identity<glm::mat4>();
        device->SetTransform(D3DTS_WORLD, reinterpret_cast<const D3DMATRIX*>(&identityMatrix));
        device->SetTransform(D3DTS_VIEW, reinterpret_cast<const D3DMATRIX*>(&identityMatrix));
        device->SetTransform(D3DTS_PROJECTION, reinterpret_cast<const D3DMATRIX*>(&identityMatrix));

        device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        device->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);

        device->SetTexture(10, depthTexture);

        // half a pixel of the back buffer, which is larger than the window while a capture renders at another size
        D3DSURFACE_DESC backBufferDesc = {};
        IDirect3DSurface9* backBuffer = nullptr;
        if (SUCCEEDED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)))
        {
            backBuffer->GetDesc(&backBufferDesc);
            backBuffer->Release();
        }
        const auto gameSize = backBufferDesc.Width > 0
                                  ? ImVec2(static_cast<float>(backBufferDesc.Width), static_cast<float>(backBufferDesc.Height))
                                  : ImGui::GetIO().DisplaySize;
        const float texelOffset[4] = { -1.0f / gameSize.x, 1.0f / gameSize.y, 0.0f, 0.0f };
        device->SetVertexDeclaration(depthPassVDecl);
        device->SetVertexShaderConstantF(0, reinterpret_cast<const float*>(&texelOffset), 1);
        device->SetVertexShader(depthPassVS);
        device->SetPixelShaderConstantF(
            0, 
            std::array<float, 4>{
                passType == Components::PassType::Depth ? 1.0f : 0.0f, 
                0.0f, 0.0f, 0.0f
            }.data(), 
            1
        );
        device->SetPixelShaderConstantF(
            4,
            std::array<float, 4>{
                passType == Components::PassType::Normal ? 1.0f : 0.0f,
                0.0f, 0.0f, 0.0f
            }.data(),
            1
        );
        device->SetPixelShaderConstantF(
            8,
            std::array<float, 4>{
                onlyDrawViewmodel ? 1.0f : 0.0f, 
                0.0f, 0.0f, 0.0f
            }.data(),
            1
        );
        device->SetPixelShaderConstantF(
            12,
            std::array<float, 4>{
                greenscreenBackground ? 1.0f : 0.0f,
                0.0f, 0.0f, 0.0f
            }.data(),
            1
        );
        //device->SetPixelShaderConstantF(
        //    1, std::array<float, 4>{passType == Components::PassType::Normal ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f}.data(), 1);
        //device->SetPixelShaderConstantB(6, (BOOL*)&onlyDrawViewmodel, 1);
        device->SetPixelShader(depthPassPS);
        device->SetStreamSource(0, depthPassVertices, 0, sizeof(Types::FSVertex));
        device->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);

        // Restore the DX9 transform
        device->SetTransform(D3DTS_WORLD, &last_world);
        device->SetTransform(D3DTS_VIEW, &last_view);
        device->SetTransform(D3DTS_PROJECTION, &last_projection);

        // Restore the DX9 state
        d3d9_state_block->Apply();
        d3d9_state_block->Release();
    }

    void GraphicsManager::DrawShaderForPassIndex(int32_t passIndex)
    {
        auto& captureSettings = Components::CaptureManager::Get().GetCaptureSettings();
        auto& pass = captureSettings.passes[passIndex];
        
        if (pass.type != Components::PassType::Default || pass.elements == Components::VisibleElements::OnlyGun)
        {
            DrawStreamsShader(
                pass.type,
                pass.elements == Components::VisibleElements::OnlyGun
            );
        }
        else if (pass.elements == Components::VisibleElements::OnlyPlayers)
        {
            // the players are drawn on black, see Components::Rendering, and the green is added after the game's
            // post-processing, which would otherwise blend it into the players
            DrawStreamsShader(pass.type, false, true);
        }
    }

    void GraphicsManager::Render()
    {
        if (ImGui::GetMainViewport()->Size.x == 0.0f || ImGui::GetMainViewport()->Size.y == 0.0f)
        {
            return;
        }

        IDirect3DDevice9* device = D3D9::GetDevice();

        // Variables used for backing up and restoring D3D9 state
        IDirect3DStateBlock9* d3d9_state_block = nullptr;
        D3DMATRIX last_world = {}, last_view = {}, last_projection = {};

        // Backup the DX9 state
        HRESULT result = device->CreateStateBlock(D3DSBT_ALL, &d3d9_state_block);
        if (FAILED(result))
        {
            throw std::runtime_error("Failed to get the D3D9 state block");
        }

        // Backup the DX9 transform (DX9 documentation suggests that it is included in the StateBlock but it doesn't
        // appear to)
        device->GetTransform(D3DTS_WORLD, &last_world);
        device->GetTransform(D3DTS_VIEW, &last_view);
        device->GetTransform(D3DTS_PROJECTION, &last_projection);
        
        SetupRenderState();

        BufferManager::Get().BindBuffers(BufferType::Static);
        // The frame's lines (rings, actor paths, the campath) go one after another into the dynamic buffers, cleared
        // once a frame: written over between draws, a line drawn before still took the next one's points from them
        // (a stray line from the ring under the selected actor to its path's last point)
        BufferManager::Get().ClearDynamicBuffers();

        objectHoveredThisFrame = false;
        mouseOverView = IsMouseOverGameView();

        const auto& activeCam = Components::CameraManager::Get().GetActiveCamera();
        const auto currentCameraMode = activeCam->GetMode();
        // (they dim while the camera flies, and come back as it stops)
        constexpr float LINE_DIM_PER_SECOND = 6.0f;
        constexpr float AXES_LENGTH = 3.0f;
        const bool moveCamera =
            currentCameraMode == Components::Camera::Mode::Free || currentCameraMode == Components::Camera::Mode::Orbit;
        const bool flying =
            moveCamera && UI::UIManager::Get().GetUIComponent(UI::Component::GameView)->HasFocus();
        lineDim = ViewLines::StepTowards(lineDim, flying ? ViewLines::WHILE_FLYING : 1.0f,
                                         Input::GetDeltaTime() * LINE_DIM_PER_SECOND);
        // None of the lines while recording: with ReShade a frame is taken after ReShade's effects, as the frame
        // ends, which is after they are drawn, so a recording in the free camera had the lights' icons and paths, the
        // rings and the gizmo in it
        const bool recording = Components::CaptureManager::Get().IsCapturing();
        if (currentCameraMode == Components::Camera::Mode::Bone && !recording)
        {
            const auto& settings = dynamic_cast<Components::BoneCamera&>(*activeCam).GetSettings();
            if (settings.showBone)
            {
                if (const auto bone = Mod::GetGameInterface()->GetTargetBone(settings.target, settings.bone))
                    DrawAxes(bone->position, glm::mat3(bone->axes), AXES_LENGTH * 1.1f);
            }
        }

        // In the free and orbit cameras actors have rings under them and can be clicked to select them, on any tab
        // (the panel then shows them on the Scene tab); while it shows the actors, the selected actor's movement and
        // gizmo. The gizmo moves one thing: the actor once one is selected, a campath node once one is clicked.
        bool actorGizmoShown = false;
        const bool actorsCamera =
            currentCameraMode == Components::Camera::Mode::Free || currentCameraMode == Components::Camera::Mode::Orbit;
        const bool actorsPage = actorsCamera && UI::UIManager::Get().IsShown(UI::Page::Actors);
        hoveredActor = std::nullopt;
        if (actorsCamera && !recording)
        {
            const auto tick = Components::Playback::GetActorTick();
            const auto selected = Components::Actors::GetSelected();
            const auto waypoint = Components::Actors::GetSelectedWaypoint();
            // (one picked on another tab is focused once the actors show)
            if (actorsPage && (selected != seenActor || waypoint != seenWaypoint))
            {
                seenActor = selected;
                seenWaypoint = waypoint;
                if (selected)
                    FocusActor();
            }

            hoveredActor = PickActor(tick);
            // (View > Rings under actors)
            if (PreferencesConfiguration::Get().showActorRings)
                DrawActorRings(selected, tick);
            DrawLights(selected);
            DrawPropPath(selected);
            DrawMortarPoints(selected);
            // (View > Other actors' paths)
            const bool otherPaths = actorsPage && PreferencesConfiguration::Get().showActorPaths;
            if (otherPaths)
                DrawOtherPaths(selected);
            if (auto actor = actorsPage && selected ? Components::Actors::GetActor(*selected) : std::nullopt)
            {
                DrawActorMovement(*actor);
                // (an attached actor moves with what it is on)
                if (gizmoTarget == GizmoTarget::Actor && actor->movement != Types::ActorMovement::Attached)
                {
                    actorGizmoShown = true;
                    const auto original = *actor;
                    DrawActorGizmo(*actor, tick);
                    if (*actor != original)
                        Components::Actors::Update(*actor);
                }
                PickPathPoint(*actor);
                PlacePathPoint(*actor);
            }
            else
            {
                placingPathPoints = false;
                hoveredPoint.reset();
                hoveredLine.reset();
            }
            if (otherPaths)
                PickOtherPathPoint(selected);
        }
        else
        {
            placingPathPoints = false;
        }
        bool nodeGizmoShown = false;
        bool nodeClicked = false;
        // the selected node as it was before this frame, while its gizmo is held
        std::optional<Types::Keyframe> heldNode;
        // nodes under the mouse count as hovered after the loop, so the selected node's gizmo, drawn in it, can be
        // grabbed close to the node too
        bool nodeHovered = false;
        constexpr auto CAMPATH_COLOR = D3DCOLOR_COLORVALUE(1, 0.8f, 0, 1);
        constexpr float NODE_HIT_RADIUS = 20.0f;

        // Only draw nodes if in free/orbit mode
        if ((currentCameraMode == Components::Camera::Mode::Free ||
             currentCameraMode == Components::Camera::Mode::Orbit) &&
            !recording)
        {
            // Draw axis when in orbit mode
            if (currentCameraMode == Components::Camera::Mode::Orbit)
            {
                const auto* orbitCam = reinterpret_cast<const Components::OrbitCamera*>(activeCam.get());

                DrawAxes(orbitCam->GetOrigin(), glm::mat3(1.0f),
                         AXES_LENGTH * glm::distance(activeCam->GetPosition(), orbitCam->GetOrigin()) / 45.0f);
            }

            auto& keyframeManager = Components::KeyframeManager::Get();
            const auto& property = keyframeManager.GetProperty(Types::KeyframeablePropertyType::CampathCamera);
            auto& nodes = keyframeManager.GetKeyframes(property);
            // (View > Campath nodes and line: hidden, they can't be clicked either)
            const bool showCampath = PreferencesConfiguration::Get().showCampath;

            // the campath's line
            if (showCampath && !nodes.empty())
                DrawLines(campath, {GetCampathLine()}, CAMPATH_COLOR, 1.0f, LineDepth::Faint);

            // Iterate through all nodes and draw each as a frame and pyramid, by its own FOV. The mouse takes a node by a
            // sphere round the node and its frame, which grows with it as it stays one size on the screen far away.
            const auto eye = GetEye();
            for (auto& node : nodes)
            {
                if (!showCampath)
                    break;

                const auto& position = node.value.cameraData.position;
                const auto translate = glm::translate(position);
                const auto rotate = glm::eulerAngleZYX(glm::radians(node.value.cameraData.rotation.y),
                                                       glm::radians(node.value.cameraData.rotation.x),
                                                       glm::radians(node.value.cameraData.rotation.z));
                const auto forward = glm::vec3(rotate[0]), left = glm::vec3(rotate[1]), up = glm::vec3(rotate[2]);
                auto size = ViewLines::GetNodeSize(eye, position);

                bool mouseIntersects =
                    mouseOverView && MouseIntersectsSphereAt(ImGui::GetIO().MousePos, position + forward * (size / 2.0f),
                                                             std::max(NODE_HIT_RADIUS, size * 0.75f));
                nodeHovered |= mouseIntersects;
                if (mouseIntersects && !heldAxis.has_value())
                    size *= 1.1f;
                const auto shape =
                    ViewLines::GetNodeShape(position, forward, left, up, node.value.cameraData.fov, size);
                DrawLines(campath, shape.lines,
                          selectedNodeId == node.id ? D3DCOLOR_COLORVALUE(1, 1, 1, 1)
                          : mouseIntersects         ? D3DCOLOR_COLORVALUE(1, 0.9f, 0.5f, 1)
                                                    : CAMPATH_COLOR,
                          1.0f, LineDepth::Faint, std::span(&shape.mark, 1));

                // (not when the click went to a gizmo's axis over it)
                if (mouseIntersects && Input::KeyDown(ImGuiKey_MouseLeft) && !heldAxis.has_value() &&
                    (selectedNodeId != node.id || gizmoTarget != GizmoTarget::CampathNode))
                {
                    if (selectedNodeId != node.id)
                        gizmoMode = GizmoMode::TranslateLocal;
                    selectedNodeId = node.id;
                    gizmoTarget = GizmoTarget::CampathNode;
                    nodeClicked = true;
                    UI::UIManager::Get().ShowPicked(UI::PanelLogic::Kind::Node);
                }

                if (mouseIntersects && selectedNodeId == node.id && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                {
                    gizmoMode = static_cast<GizmoMode>((gizmoMode + 1) % GizmoMode::Count);
                }

                if (selectedNodeId == node.id && !actorGizmoShown)
                {
                    nodeGizmoShown = true;
                    const auto before = node;
                    switch (gizmoMode)
                    {
                        case TranslateGlobal:
                        case TranslateLocal:
                            DrawTranslationGizmo(
                                node.value.cameraData.position, translate,
                                gizmoMode == GizmoMode::TranslateLocal ? rotate : glm::eulerAngleZYX(.0f, .0f, .0f));
                            break;
                        case Rotate:
                            DrawRotationGizmo(node.value.cameraData.rotation, translate);
                            break;
                    }
                    if (heldAxis.has_value())
                        heldNode = before;
                }
            }

            objectHoveredThisFrame |= nodeHovered;

            // an actor under the mouse (and not under a node or a gizmo's axis) is selected by a click
            if (hoveredActor)
            {
                if (Input::KeyDown(ImGuiKey_MouseLeft) && !heldAxis.has_value() && !nodeClicked &&
                    !objectHoveredThisFrame)
                {
                    Components::Actors::Select(*hoveredActor);
                    FocusActor();
                    UI::UIManager::Get().ShowPicked(UI::PanelLogic::Kind::Actor);
                }
                objectHoveredThisFrame = true;
            }
        }

        gizmoShown = actorGizmoShown || nodeGizmoShown;
        // (a gizmo lets go of an axis as it's drawn: with none drawn, the node gone or another camera, nothing would)
        if (!gizmoShown)
            heldAxis = std::nullopt;
        if (const auto drag = nodeDrag.Frame(heldNode ? &*heldNode : nullptr))
            Components::KeyframeManager::Get().EndNodeDrag(*drag);

        auto captureMenu = UI::UIManager::Get().GetUIComponent<UI::CaptureMenu>(UI::Component::Component::CaptureMenu);
        if (captureMenu->GetDisplayPassIndex().has_value())
        {
            DrawShaderForPassIndex(captureMenu->GetDisplayPassIndex().value());
		}

        // Restore the DX9 transform
        device->SetTransform(D3DTS_WORLD, &last_world);
        device->SetTransform(D3DTS_VIEW, &last_view);
        device->SetTransform(D3DTS_PROJECTION, &last_projection);

        // Restore the DX9 state
        d3d9_state_block->Apply();
        d3d9_state_block->Release();
    }
}  // namespace IWXMVM
