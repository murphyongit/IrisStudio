#pragma once
#include "StdInclude.hpp"

#include "Graphics/Resource.hpp"
#include "Resources.hpp"
#include "Types/Keyframe.hpp"
#include "Types/Actor.hpp"
#include "Components/CaptureManager.hpp"
#include "Components/KeyframeLogic.hpp"
#include "Graphics/GizmoState.hpp"

namespace IWXMVM::GFX
{
    INCBIN_EXTERN(GIZMO_TRANSLATE_MODEL);
    INCBIN_EXTERN(GIZMO_ROTATE_MODEL);

    enum GizmoMode
    {
        TranslateLocal,
        TranslateGlobal,
        Rotate,
        Count
    };

    struct TranslationGizmoData
    {
        std::int32_t axisIndex{};
        glm::vec3 objectOffset{};
    };

    class GraphicsManager
    {
       public:
        static GraphicsManager& Get()
        {
            static GraphicsManager instance;
            return instance;
        }

        GraphicsManager(GraphicsManager const&) = delete;
        void operator=(GraphicsManager const&) = delete;

        void Initialize();
        void Uninitialize();
        void Render();
        void DrawShaderForPassIndex(int32_t passIndex);

        std::optional<int32_t> GetSelectedNodeId() const { return selectedNodeId; }
        bool WasObjectHoveredThisFrame() const { return objectHoveredThisFrame; }
        bool IsMouseOverView() const { return mouseOverView; }
        bool WasObjectDraggedThisFrame() const { return heldAxis.has_value(); }
        // whether a gizmo was drawn last frame
        bool IsGizmoShown() const { return gizmoShown; }
        // how strong the view's lines are now, 1 but while the camera flies
        float GetLineDim() const { return lineDim; }
        GizmoTarget GetGizmoTarget() const { return gizmoTarget; }
        // the actor under the mouse (free and orbit cameras), which a click selects
        std::optional<std::uint32_t> GetHoveredActor() const { return hoveredActor; }
        // the selected actor's path point under the mouse (a click selects it, a right-click opens its menu), else the
        // line from one (a right-click opens its menu)
        std::optional<std::size_t> GetHoveredPoint() const
        {
            return hoveredPoint;
        }
        std::optional<std::size_t> GetHoveredLine() const
        {
            return hoveredLine;
        }
        // the actor whose name the mouse is over in the actors list: lit up like one under the mouse in the view
        void SetListHoveredActor(std::optional<std::uint32_t> id)
        {
            listHoveredActor = id;
        }
        // a stretch of an actor's path the timeline's path bar has the mouse over: drawn lit up, with where the actor
        // is at the tick under the mouse
        void SetHighlightedStretch(std::optional<std::pair<std::uint32_t, std::size_t>> stretch,
                                   std::optional<std::uint32_t> tick = std::nullopt)
        {
            highlightedStretch = stretch;
            highlightedTick = tick;
        }
        // while points are placed: the ground under the mouse as a plane through the last point guesses it, how long
        // the stretch to it takes and at which gait, for the hint by the mouse
        struct PlacingPreview
        {
            glm::vec2 mouse;
            float seconds = 0.0f;
            std::string gait;
        };
        std::optional<PlacingPreview> GetPlacingPreview() const
        {
            return placingPreview;
        }

        // makes the gizmo move the selected actor (it does once one is selected, until a campath node is clicked)
        void FocusActor()
        {
            gizmoTarget = GizmoTarget::Actor;
            heldAxis = std::nullopt;
        }

        // a campath node selected elsewhere (the timeline): the gizmo moves it
        void SelectNode(std::int32_t id)
        {
            if (selectedNodeId != id)
                gizmoMode = GizmoMode::TranslateLocal;
            selectedNodeId = id;
            gizmoTarget = GizmoTarget::CampathNode;
            heldAxis = std::nullopt;
        }

        void ResetSelection()
        {
            selectedNodeId = std::nullopt;
            heldAxis = std::nullopt;
            gizmoTarget = GizmoTarget::None;
        }

        GizmoMode GetGizmoMode() const { return gizmoMode; }
        void SetGizmoMode(GizmoMode mode) { gizmoMode = mode; }

        // While on, a click on the world adds a point to the selected actor's path where it hits the ground. Goes off
        // with Escape, or when the actor doesn't walk a path or the actors tab closes.
        bool IsPlacingPathPoints() const { return placingPathPoints; }
        void SetPlacingPathPoints(bool placing)
        {
            placingPathPoints = placing;
            placingGait.reset();
        }
        // while placing: the gait the next stretch goes at, picked with 1, 2, 3;
        // none for the path's last
        std::optional<std::pair<Types::ActorGait, bool>> placingGait;

       private:
        GraphicsManager()
            : gizmo_translate_x(GIZMO_TRANSLATE_MODEL_data, GIZMO_TRANSLATE_MODEL_size),
              gizmo_translate_y(GIZMO_TRANSLATE_MODEL_data, GIZMO_TRANSLATE_MODEL_size),
              gizmo_translate_z(GIZMO_TRANSLATE_MODEL_data, GIZMO_TRANSLATE_MODEL_size),
              gizmo_rotate_x(GIZMO_ROTATE_MODEL_data, GIZMO_ROTATE_MODEL_size),
              gizmo_rotate_y(GIZMO_ROTATE_MODEL_data, GIZMO_ROTATE_MODEL_size),
              gizmo_rotate_z(GIZMO_ROTATE_MODEL_data, GIZMO_ROTATE_MODEL_size),
              campath(),
              actorPath(),
              actorRing(),
              lightLines(),
              gizmoLines()
        {
        }

        bool MouseIntersects(ImVec2 mousePos, Mesh& mesh, glm::mat4 model);
        bool MouseIntersectsSphereAt(ImVec2 mousePos, glm::vec3 pos, float radius);
        void DrawGizmoComponent(Mesh& mesh, glm::mat4 model, int32_t axisIndex);
        // scale 3 is the arrows' 48 units
        void DrawTranslationGizmo(glm::vec3& position, glm::mat4 translation, glm::mat4 rotation, float scale = 3.0f);
        void DrawRotationGizmo(glm::vec3& rotation, glm::mat4 translation, float scale = 3.0f);

        void DrawStreamsShader(Components::PassType passType, bool onlyDrawViewmodel, bool greenscreenBackground = false) const;
        
        std::vector<glm::vec3> GetCampathLine();
        void DrawActorMovement(const Types::Actor& actor);
        void DrawLights(std::optional<std::uint32_t> selected);
        void DrawOtherPaths(std::optional<std::uint32_t> selected);
        void DrawActorGizmo(Types::Actor& actor, std::uint32_t tick);
        void DrawLightGizmo(Types::Actor& actor);
        void DrawPropPath(std::optional<std::uint32_t> selected);
        void DrawMortarPoints(std::optional<std::uint32_t> selected);
        void DrawPropGizmo(Types::Actor& actor);
        std::optional<std::uint32_t> PickActor(std::uint32_t tick) const;
        void DrawActorRings(std::optional<std::uint32_t> selected, std::uint32_t tick);
        // Over the world, or at full strength where nothing is in front of them and faint where something is (AY1)
        enum class LineDepth
        {
            Over,
            Faint,
        };
        // Lines of the view's width times a weight, with a dark edge, and solid triangles of the colour, at the view's
        // strength
        void DrawLines(Mesh& mesh, const std::vector<std::vector<glm::vec3>>& lines, D3DCOLOR color, float weight = 1.0f,
                       LineDepth depth = LineDepth::Over, std::span<const std::array<glm::vec3, 3>> fills = {});
        // a ring of a point on a path, facing the view, filled when it is selected
        void DrawPoint(glm::vec3 point, D3DCOLOR color, bool filled, float radiusPixels);
        // red, green and blue lines through a point along its axes, to a length either way
        void DrawAxes(glm::vec3 position, const glm::mat3& axes, float length);
        void SetupRenderState() const noexcept;

        void CreateGraphicsResources();
        void DestroyGraphicsResources();
        IDirect3DPixelShader9* pixelShader = nullptr;
        IDirect3DVertexShader9* vertexShader = nullptr;
        IDirect3DVertexDeclaration9* vertexDeclaration = nullptr;

        void CreateDepthPassResources();
        void DestroyDepthPassResources();
        IDirect3DPixelShader9* depthPassPS = nullptr;
        IDirect3DVertexShader9* depthPassVS = nullptr;
        IDirect3DVertexDeclaration9* depthPassVDecl = nullptr;
        IDirect3DVertexBuffer9* depthPassVertices = nullptr;

        Mesh gizmo_translate_x;
        Mesh gizmo_translate_y;
        Mesh gizmo_translate_z;
        Mesh gizmo_rotate_x;
        Mesh gizmo_rotate_y;
        Mesh gizmo_rotate_z;
        Mesh campath;
        Mesh actorPath;
        Mesh actorRing;
        Mesh lightLines;
        Mesh gizmoLines;

        std::optional<int32_t> selectedNodeId = std::nullopt;
        std::optional<TranslationGizmoData> heldAxis = std::nullopt;
        // a node's gizmo held: one step to undo when it's let go
        KeyframeLogic::KeyDrag nodeDrag;
        bool objectHoveredThisFrame = false;
        // the mouse is over the view's image (not a panel, the strip or a message) this frame
        bool mouseOverView = false;
        GizmoMode gizmoMode = GizmoMode::TranslateLocal;
        GizmoTarget gizmoTarget = GizmoTarget::None;
        bool gizmoShown = false;
        float lineDim = 1.0f;
        // the actor selection last seen, to move the gizmo to an actor when one is selected
        std::optional<std::uint32_t> seenActor;
        std::optional<std::size_t> seenWaypoint;
        std::optional<std::uint32_t> hoveredActor;
        std::optional<std::size_t> hoveredPoint;
        std::optional<std::size_t> hoveredLine;
        std::optional<std::uint32_t> listHoveredActor;
        std::optional<std::pair<std::uint32_t, std::size_t>> highlightedStretch;
        std::optional<std::uint32_t> highlightedTick;
        std::optional<PlacingPreview> placingPreview;
        bool placingPathPoints = false;

        void PickPathPoint(const Types::Actor& actor);
        void PickOtherPathPoint(std::optional<std::uint32_t> selected);

        void PlacePathPoint(const Types::Actor& actor);
    };
}  // namespace IWXMVM
