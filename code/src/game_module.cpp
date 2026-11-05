#include <snk/application/app_module_entry.h>
#include <snk/game/gs_game_module.h>
#include <snk/ecs/ecs_system.h>
#include <snk/scene/scn_model.h>
#include <snk/scene/scn_primitives.h>
#include <snk/render/geom/rnd_geometry_desc.h>
#include <snk/scene/scn_time.h>
#include <snk/scene/level/scn_context_set.h>
#include <snk/scene/level/scn_prefab_desc.h>
#include <snk/scene/level/scn_node_index.h>
#include <snk/common/eng_transform_3d.hpp>
#include <snk/input/inp_events.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <entt/entt.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {
using cell = glm::ivec2;
enum class result { playing, wall, self, won };
struct snake_rules {
    static constexpr int goal = 12;
    std::deque<cell> body{{0,3},{0,4},{0,5}};
    cell direction{0,-1}, pending{0,-1}, food{0,0};
    int score=0;
    bool turn_queued=false;
    result state=result::playing;
    std::mt19937 rng{1015};
    bool occupied(cell p) const { return std::find(body.begin(),body.end(),p)!=body.end(); }
    bool choose_food() {
        std::vector<cell> free;
        for(int z=-5;z<=5;++z) for(int x=-7;x<=7;++x)
            if(!occupied({x,z})) free.push_back({x,z});
        if(free.empty()) { state=result::won; return false; }
        std::uniform_int_distribution<std::size_t> pick(0,free.size()-1);
        food=free[pick(rng)]; return true;
    }
    bool turn(cell desired) {
        if(state!=result::playing || turn_queued || desired==-direction) return false;
        if(std::abs(desired.x)+std::abs(desired.y)!=1 || desired==direction) return false;
        pending=desired; turn_queued=true; return true;
    }
    bool step() {
        if(state!=result::playing) return false;
        direction=pending; turn_queued=false;
        const cell next=body.front()+direction;
        if(next.x < -7 || next.x > 7 || next.y < -5 || next.y > 5) { state=result::wall; return false; }
        const bool grows=next==food;
        // The last segment vacates its cell on a non-growing move.
        const std::size_t blocked=body.size()-(grows?0:1);
        for(std::size_t i=0;i<blocked;++i) if(body[i]==next) { state=result::self; return false; }
        body.push_front(next);
        if(grows) { ++score; if(score>=goal) state=result::won; }
        else body.pop_back();
        return grows;
    }
};
bool rules_self_test() {
    bool ok=true;
    { snake_rules s; ok &= !s.turn({0,1}); ok &= s.turn({1,0}); ok &= !s.turn({0,1}); s.step(); ok &= s.body.front()==cell{1,3}; }
    { snake_rules s; s.body={{7,0},{6,0},{5,0}}; s.direction=s.pending={1,0}; s.step(); ok &= s.state==result::wall; }
    { snake_rules s; s.body={{0,0},{0,1},{1,1},{1,0},{1,-1},{0,-1}}; s.direction=s.pending={1,0}; s.food={-5,-5}; s.step(); ok &= s.state==result::self; }
    { snake_rules s; s.body={{0,0},{0,1},{1,1},{1,0}}; s.direction=s.pending={1,0}; s.food={-5,-5}; s.step(); ok &= s.state==result::playing && s.body.front()==cell{1,0} && s.body.size()==4; }
    { snake_rules s; s.food={0,2}; ok &= s.step(); ok &= s.score==1 && s.body.size()==4; for(int i=0;i<50;++i) { ok &= s.choose_food(); ok &= !s.occupied(s.food); } }
    { snake_rules s; s.score=11; s.food={0,2}; s.step(); ok &= s.state==result::won && s.score==12; auto old=s.body; s.step(); ok &= s.body==old; }
    egLOG("SnakeDemo", "Rules self-test: {} (turn queue, reverse, wall, self collision, vacating tail, growth, free food, victory)",ok?"PASS":"FAIL");
    return ok;
}
class crystal_run final : public ecs::system_interface {
public:
    void run(entt::registry& reg,const inp::input_state& input,const scn::delta_time& delta,
             ecs::event<scn::transform_updated>& changed) {
        auto* set=scn::context_set::current();
        if(!set || !set->layer()) return;
        auto& layer=*set->layer();
        if(!m_requested) {
            m_character=layer.resources().require<scn::prefab_desc>(res::tag("res://assets/character/character.prefab.desc"));
            m_coin=layer.resources().require<scn::prefab_desc>(res::tag("res://assets/coin/coin.prefab.desc"));
            make_result_shaders(layer);
            make_fireworks(layer);
            m_win_material = make_color(layer, "win-green", "[0.05,1.0,0.15,1.0]");
            m_lose_material = make_color(layer, "lose-red", "[1.0,0.035,0.025,1.0]");
            m_requested=true;
        }
        if(!m_character.is_ready() || !m_coin.is_ready() || !m_win_material.is_ready() || !m_lose_material.is_ready() || !m_fire_material.is_ready() || !m_fire_geometry.is_ready()) return;
        const auto down=[&](inp::KEYBOARD_BUTTONS k){return input.keyboard[static_cast<std::size_t>(k)];};
        const bool restart=down(inp::KEYBOARD_BUTTONS::R);
        if(!m_initialized || (restart && !m_restart_down)) {
            reset(layer,reg,changed); m_restart_down=restart; return;
        }
        m_restart_down=restart;
        const bool preview=down(inp::KEYBOARD_BUTTONS::F);
        if(preview && !m_preview_down)m_preview=!m_preview;
        m_preview_down=preview;
        if(down(inp::KEYBOARD_BUTTONS::W)||down(inp::KEYBOARD_BUTTONS::UP)) m_game.turn({0,-1});
        else if(down(inp::KEYBOARD_BUTTONS::S)||down(inp::KEYBOARD_BUTTONS::DOWN)) m_game.turn({0,1});
        else if(down(inp::KEYBOARD_BUTTONS::A)||down(inp::KEYBOARD_BUTTONS::LEFT)) m_game.turn({-1,0});
        else if(down(inp::KEYBOARD_BUTTONS::D)||down(inp::KEYBOARD_BUTTONS::RIGHT)) m_game.turn({1,0});
        const float dt=std::clamp(delta.dt,0.f,0.1f);
        m_time+=dt;
        if(m_delay>0.f) m_delay=std::max(0.f,m_delay-dt);
        else if(m_game.state==result::playing) {
            m_accumulator+=dt;
            if(m_accumulator>=m_tick) {
                m_accumulator-=m_tick;
                m_previous=m_game.body;
                const bool grew=m_game.step();
                if(grew) {
                    m_segments.push_back(spawn(layer,reg,*m_character.get(),"SnakeTail"+std::to_string(m_segments.size())));
                    if(m_game.state==result::playing) {
                        erase_tree(reg,m_food_entity);
                        m_game.choose_food();
                        m_food_entity=spawn(layer,reg,*m_coin.get(),"SnakeFood"+std::to_string(++m_food_serial));
                    }
                    egLOG("SnakeDemo","Score {}/{}; body segments={}; runtime coin #{}",m_game.score,snake_rules::goal,m_game.body.size(),m_food_serial);
                }
                if(m_game.state!=result::playing) {
                    m_accumulator=0.f;
                    make_banner(layer,reg);
                    egLOG("SnakeDemo","{}; score {}/{}. R / Cyrillic K: restart",m_game.state==result::won?"VICTORY":m_game.state==result::wall?"GAME OVER: wall":"GAME OVER: own tail",m_game.score,snake_rules::goal);
                }
            }
        }
        const float alpha=m_game.state==result::playing?std::clamp(m_accumulator/m_tick,0.f,1.f):1.f;
        auto view=reg.view<scn::name_component,scn::local_transform>();
        for(auto&& [entity,name,transform]:view.each()) {
            if(name.name=="Game Camera") {
                transform.local=glm::translate(glm::mat4{1.f},glm::vec3{0.f,15.f,13.f})
                    *glm::rotate(glm::mat4{1.f},glm::radians(-48.f),glm::vec3{1.f,0.f,0.f});changed.emit(entity);
            } else if(name.name=="Player") draw_segment(reg,entity,0,alpha,changed);
            else if(name.name.starts_with("Crystal") || name.name=="ExitSignal") {
                transform.local=glm::scale(glm::mat4{1.f},glm::vec3{0.f});changed.emit(entity);
            }
        }
        for(std::size_t i=0;i<m_segments.size();++i) draw_segment(reg,m_segments[i],i+1,alpha,changed);
        place(reg,m_food_entity,glm::vec2(m_game.food),m_time, m_game.state==result::playing?0.65f:0.f,0.f,changed);
        if(reg.valid(m_fire_entity)) {
            const bool visible=m_preview || m_game.state==result::won;
            reg.get<scn::mesh_component>(m_fire_entity).mesh.ind_end=visible?36:0;
            m_fire_material.get()->shininess=m_time;
            reg.get<scn::material_desc_component>(m_fire_entity).dirty=true;
        }
        if(!m_banner.empty()) {
            auto material=m_game.state==result::won?m_win_material:m_lose_material;
            material.get()->shininess=m_time;
            for(const auto& item:m_banner)tint_banner(reg,item.first,material);
        }
        for(const auto& item:m_banner) place(reg,item.first,item.second,std::sin(m_time*1.2f+item.second.x*0.3f)*0.22f,1.0f,0.f,changed);
    }
private:
    template<class T>
    static void forget_event(entt::registry& reg, entt::entity entity) {
        if (auto* event = reg.ctx().find<ecs::event<T>>()) {
            const auto pending = event->list();
            event->clear();
            for (auto item : pending) if (item != entity && reg.valid(item)) event->emit(item);
        }
    }
    static void erase_tree(entt::registry& reg, entt::entity entity) {
        if (!reg.valid(entity)) return;
        if (auto* children = reg.try_get<scn::children_component>(entity)) {
            const auto copy = children->children;
            for (auto child : copy) erase_tree(reg, child);
        }
        forget_event<scn::transform_updated>(reg, entity);
        forget_event<scn::hierarchy_updated>(reg, entity);
        reg.destroy(entity);
    }
    static entt::entity spawn(scn::scene_layer& layer, entt::registry& reg,
                              const scn::prefab_desc& prefab, const std::string& name) {
        const auto entity = reg.create();
        scn::node_index::get(reg);
        // Runtime roots use a separate path prefix; prefab children retain their asset ids.
        reg.emplace<scn::node_id_component>(entity, std::vector<std::uint64_t>{0, std::uint64_t(entt::to_integral(entity))});
        reg.emplace<scn::local_transform>(entity);
        scn::assemble_prefab(layer.assembler(), reg, entity, prefab, name, res::tag{});
        return entity;
    }
    static void place(entt::registry& reg, entt::entity entity, glm::vec2 p, float heading,
                      float scale, float y, ecs::event<scn::transform_updated>& changed) {
        if (!reg.valid(entity)) return;
        auto& local = reg.get_or_emplace<scn::local_transform>(entity);
        local.local = glm::translate(glm::mat4{1.f}, glm::vec3{p.x, y, p.y})
            * glm::rotate(glm::mat4{1.f}, heading, glm::vec3{0.f, 1.f, 0.f})
            * glm::scale(glm::mat4{1.f}, glm::vec3{scale});
        changed.emit(entity);
    }

    void draw_segment(entt::registry& reg,entt::entity entity,std::size_t i,float alpha,ecs::event<scn::transform_updated>& changed) {
        const cell target=m_game.body[std::min(i,m_game.body.size()-1)];
        const cell previous=m_previous[std::min(i,m_previous.size()-1)];
        const glm::vec2 p=glm::mix(glm::vec2(previous),glm::vec2(target),alpha);
        cell heading=i==0?m_game.direction:m_game.body[i-1]-target;
        if(heading==cell{0,0}) heading=m_game.direction;
        const float angle=std::atan2(float(heading.x),float(heading.y));
        place(reg,entity,p,angle,i==0?0.65f:0.6f,0.f,changed);
    }
    void reset(scn::scene_layer& layer,entt::registry& reg,ecs::event<scn::transform_updated>& changed) {
        erase_tree(reg,m_food_entity);
        for(auto entity:m_segments) erase_tree(reg,entity);
        for(const auto& item:m_banner) erase_tree(reg,item.first);
        m_segments.clear();m_banner.clear();m_game=snake_rules{};m_previous=m_game.body;
        m_time=0.f;m_accumulator=0.f;m_delay=2.f;m_food_serial=1;
        for(std::size_t i=1;i<m_game.body.size();++i) {
            auto entity=spawn(layer,reg,*m_character.get(),"SnakeTail"+std::to_string(i));m_segments.push_back(entity);
            place(reg,entity,glm::vec2(m_game.body[i]),0.f,0.6f,0.f,changed);
        }
        m_food_entity=spawn(layer,reg,*m_coin.get(),"SnakeFood1");
        place(reg,m_food_entity,glm::vec2(m_game.food),0.f,0.65f,0.f,changed);
        if(!reg.valid(m_fire_entity)) {
            m_fire_entity=reg.create();
            reg.emplace<scn::name_component>(m_fire_entity,scn::name_component{"Fireworks Volume Cube"});
            reg.emplace<scn::local_transform>(m_fire_entity,glm::translate(glm::mat4{1.f},glm::vec3{0.f,4.f,0.f})*glm::scale(glm::mat4{1.f},glm::vec3{5.f,3.7f,3.f}));
            reg.emplace<scn::world_transform>(m_fire_entity);
            reg.emplace<scn::depth_level>(m_fire_entity);
            reg.emplace<scn::mesh_component>(m_fire_entity,scn::mesh_component{scn::mesh_view{0,8,0,36}});
            reg.emplace<scn::geometry_component>(m_fire_entity,scn::geometry_component{m_fire_geometry.get_tag()});
            reg.emplace<scn::material_desc_component>(m_fire_entity,scn::material_desc_component{m_fire_material});
            changed.emit(m_fire_entity);
        }
        reg.get<scn::mesh_component>(m_fire_entity).mesh.ind_end=0;
        m_preview=false;
        m_initialized=true;
        egLOG("SnakeDemo","NEW GAME: auto move after 2 seconds; WASD/arrows turn; collect 12 coins to WIN; walls/tail lose; R restarts");
    }
    static std::array<std::string_view,5> glyph(char c) {
        switch(c) {
        case 'W':return {"10101","10101","10101","10101","01010"};
        case 'I':return {"111","010","010","010","111"};
        case 'N':return {"1001","1101","1101","1011","1001"};
        case 'L':return {"100","100","100","100","111"};
        case 'O':return {"111","101","101","101","111"};
        case 'S':return {"111","100","111","001","111"};
        default:return {"111","100","110","100","111"};
        }
    }
    void make_fireworks(scn::scene_layer& layer) {
        const std::string root=gs::module_resource_prefix("game");
        store_text(layer,res::tag(root+"shaders/fireworks-v4.vert"),R"GLSL(#version 450 core
layout(location=0)in vec3 aPos;
SNK_UBO(0,0) uniform Matrices {mat4 projection;mat4 view;float time;vec3 cameraPosition;};
SNK_UBO(3,0) uniform ObjectData {mat4 world;uint boneOffset;};
out vec3 localSurface;
out vec3 localCamera;
void main(){localSurface=aPos;localCamera=(inverse(world)*vec4(cameraPosition,1)).xyz;gl_Position=projection*view*world*vec4(aPos,1);}
)GLSL");
        store_text(layer,res::tag(root+"shaders/fireworks-v4.frag"),R"GLSL(#version 450 core
SNK_UBO(0,0) uniform Matrices {mat4 projection;mat4 view;float time;vec3 cameraPosition;};
SNK_UBO(2,0) uniform MaterialParams {vec4 diffuseColor;vec4 emissiveColor;float shininess;};
#define time shininess
in vec3 localSurface;
in vec3 localCamera;
layout(location=0)out vec4 fragColor;
layout(location=1)out float fragWeight;
float hash(float x){return fract(sin(x*127.1+311.7)*43758.5453);}
float spark(vec3 p,vec3 ro,vec3 rd,float nearT,float farT,float radius){
 float t=clamp(dot(p-ro,rd),nearT,farT);float d=length(ro+rd*t-p);
 return exp(-d*d/(radius*radius))+0.07*exp(-d*d/(radius*radius*14.0));
}
void main(){
 vec3 ro=localCamera,rd=normalize(localSurface-ro);
 vec3 a=(-vec3(1)-ro)/rd,b=(vec3(1)-ro)/rd;
 vec3 lo=min(a,b),hi=max(a,b);
 float nearT=max(max(lo.x,lo.y),max(lo.z,0.0));
 float farT=min(min(hi.x,hi.y),hi.z);
 if(farT<=nearT)discard;
 vec3 energy=vec3(0);
 for(int burst=0;burst<4;++burst){
  float seed=float(burst)*31.7;
  float cycle=floor((time+float(burst)*1.35)/4.5);
  float age=mod(time+float(burst)*1.35,4.5);
  float id=seed+cycle*9.3;
  vec3 origin=vec3((hash(id+1)-0.5)*1.0,0.2+hash(id+2)*0.35,(hash(id+3)-0.5)*0.8);
  vec3 hue=mix(vec3(0.05,0.65,1.0),vec3(1.0,0.2,0.75),hash(id+4));
  if(age<1.0){
   vec3 p=mix(vec3(origin.x,-0.95,origin.z),origin,age);
   energy+=vec3(1,0.65,0.18)*spark(p,ro,rd,nearT,farT,0.016)*1.7;
   for(int k=1;k<5;++k){vec3 trail=p-vec3(0,float(k)*0.035,0);energy+=vec3(1,0.28,0.03)*spark(trail,ro,rd,nearT,farT,0.009)*0.3;}
  }else{
   float t=age-1.0;
   float fade=pow(max(0.0,1.0-t/2.7),1.5);
   for(int i=0;i<24;++i){
    float f=float(i);float y=1.0-2.0*(f+0.5)/24.0;float angle=f*2.399963+hash(id)*6.283;
    vec3 dir=vec3(sqrt(1.0-y*y)*cos(angle),y,sqrt(1.0-y*y)*sin(angle));
    vec3 velocity=dir*(0.38+hash(id+f+5)*0.13);
    vec3 p=origin+velocity*t-vec3(0,0.11*t*t,0);
    if(any(greaterThan(abs(p),vec3(0.97))))continue;
    float twinkle=0.72+0.28*sin(time*14.0+f*7.0);
    float light=spark(p,ro,rd,nearT,farT,0.018)*fade*twinkle;
    float past=max(0.0,t-0.085);
    vec3 tail=origin+velocity*past-vec3(0,0.11*past*past,0);
    light+=spark(tail,ro,rd,nearT,farT,0.012)*fade*0.35;
    energy+=mix(hue,vec3(1,0.82,0.4),0.18)*light*2.4;
   }
   energy+=vec3(1,0.8,0.4)*spark(origin,ro,rd,nearT,farT,0.055)*exp(-t*9.0)*3.0;
  }
 }
 float intensity=max(energy.r,max(energy.g,energy.b));
 float alpha=clamp(1.0-exp(-intensity*1.4),0.0,0.96);
 if(alpha<0.006)discard;
 vec3 color=energy/max(intensity,0.001);
 float weight=clamp(pow(min(1.0,alpha*10.0)+0.01,3.0)*1e8*pow(1.0-gl_FragCoord.z*0.9,3.0),1e-2,3e3);
 fragColor=vec4(color*alpha,alpha)*weight;
 fragWeight=alpha;
}
)GLSL");
        const auto cube=scn::generate_cube();
        std::string json=R"JSON({"__type":"geometry_desc","layout":{"position":"vec3"},"indices":[)JSON";
        for(std::size_t i=0;i<cube.indices.size();++i){if(i)json+=",";json+=std::to_string(cube.indices[i]);}
        json+="],\"vertices\":[";
        bool first=true;
        for(const auto& vertex:cube.vertices){
            const auto* bytes=reinterpret_cast<const unsigned char*>(&vertex.position);
            for(std::size_t i=0;i<sizeof(glm::vec3);++i){if(!first)json+=",";first=false;json+=std::to_string(bytes[i]);}
        }
        json+="],\"bounds\":[]}";
        const res::tag geometry(root+"geometry/fireworks-cube-v1.desc");
        store_text(layer,geometry,json);
        m_fire_geometry=layer.resources().require<rnd::geometry_desc>(geometry);
        const res::tag material(root+"materials/fireworks-v4.desc");
        store_text(layer,material,R"JSON({"__type":"material_desc","__parent":"engine://base_material.desc","shader_vertex":"memory://modules/game/shaders/fireworks-v4.vert","shader_fragment":"memory://modules/game/shaders/fireworks-v4.frag","defines":[],"samplers":[],"queue":"transparent"})JSON");
        m_fire_material=layer.resources().require<scn::material_desc>(material);
    }
    static void store_text(scn::scene_layer& layer,const res::tag& tag,std::string_view text) {
        std::vector<std::byte> bytes;bytes.reserve(text.size());
        for(unsigned char c:text)bytes.push_back(static_cast<std::byte>(c));
        layer.resources().store_sync(tag,bytes);
    }
    static void make_result_shaders(scn::scene_layer& layer) {
        store_text(layer,res::tag(gs::module_resource_prefix("game")+"shaders/result-metal-v2.vert"),R"GLSL(#version 450 core
layout(location=0)in vec3 aPos;
layout(location=1)in vec3 aNormal;
layout(location=2)in vec2 aTexCoords;
SNK_UBO(0,0) uniform Matrices {mat4 projection;mat4 view;float time;vec3 cameraPosition;};
SNK_UBO(3,0) uniform ObjectData {mat4 world;uint boneOffset;};
out vec3 resultPosition;
out vec3 resultNormal;
out vec2 resultUV;
void main(){
    vec4 p=world*vec4(aPos,1.0);
    resultPosition=p.xyz;
    resultNormal=normalize(transpose(inverse(mat3(world)))*aNormal);
    resultUV=aTexCoords;
    gl_Position=projection*view*p;
}
)GLSL");
        store_text(layer,res::tag(gs::module_resource_prefix("game")+"shaders/result-metal-v2.frag"),R"GLSL(#version 450 core
SNK_UBO(0,0) uniform Matrices {mat4 projection;mat4 view;float time;vec3 cameraPosition;};
SNK_UBO(2,0) uniform MaterialParams {vec4 diffuseColor;vec4 emissiveColor;float shininess;};
SNK_TEX(2,0) uniform sampler2D coinTexture;
#define time shininess
in vec3 resultPosition;
in vec3 resultNormal;
in vec2 resultUV;
out vec4 fragColor;
void main(){
    vec3 n=normalize(resultNormal);
    vec3 v=normalize(cameraPosition-resultPosition);
    if(dot(n,v)<0.0)n=-n;
    vec3 l=normalize(vec3(-0.45,1.0,0.65));
    vec3 h=normalize(l+v);
    vec3 tex=texture(coinTexture,resultUV).rgb;
    float engraving=clamp(dot(tex,vec3(0.299,0.587,0.114))*1.25,0.14,1.0);
    bool win=diffuseColor.g>diffuseColor.r;
    vec3 tint=win?vec3(0.08,0.72,0.22):vec3(0.76,0.055,0.025);
    vec3 metal=mix(vec3(0.95,0.63,0.16),tint,0.82);
    float lambert=max(dot(n,l),0.0);
    float highlight=pow(max(dot(n,h),0.0),48.0);
    float rim=pow(1.0-max(dot(n,v),0.0),3.0);
    float phase=resultPosition.x*0.65+resultPosition.z*0.35;
    float pulse=0.5+0.5*sin(time*(win?1.4:2.2)-phase);
    float sweep=pow(0.5+0.5*sin(resultPosition.x*1.15+resultPosition.z*0.4-time*1.7),18.0);
    vec3 color=metal*engraving*(0.28+0.85*lambert);
    color+=mix(vec3(1.0,0.82,0.44),vec3(0.8,1.0,0.9),win?0.65:0.0)*highlight*0.85;
    color+=tint*rim*(0.18+0.2*pulse);
    if(win)color+=vec3(0.45,1.0,0.65)*sweep*0.24;
    else color+=vec3(1.0,0.075,0.015)*pulse*0.12;
    fragColor=vec4(color,1.0);
}
)GLSL");
    }
    static res::res_handle<scn::material_desc> make_color(scn::scene_layer& layer, const std::string& name, const std::string& rgba) {
        const res::tag tag(gs::module_resource_prefix("game") + "materials/" + name + "-metal-v2.desc");
        const std::string text = R"JSON({"__type":"material_desc","__parent":"engine://base_material.desc","defines":[],"samplers":["res://assets/coin/textures/Textures/colormap.png.txm.desc"],"shader_vertex":"memory://modules/game/shaders/result-metal-v2.vert","shader_fragment":"memory://modules/game/shaders/result-metal-v2.frag","uniforms":{"albedo":)JSON" + rgba + R"JSON(,"emissive":[0,0,0,0],"shininess":48},"queue":"opaque"})JSON";
        std::vector<std::byte> bytes;
        bytes.reserve(text.size());
        for (unsigned char c : text) bytes.push_back(static_cast<std::byte>(c));
        layer.resources().store_sync(tag, bytes);
        return layer.resources().require<scn::material_desc>(tag);
    }
    static void tint_banner(entt::registry& reg, entt::entity entity, const res::res_handle<scn::material_desc>& material) {
        if (auto* component = reg.try_get<scn::material_desc_component>(entity)) {
            component->mlt_desc = material;
            component->dirty = true;
        }
        if (auto* children = reg.try_get<scn::children_component>(entity))
            for (auto child : children->children) tint_banner(reg, child, material);
    }
    void make_banner(scn::scene_layer& layer,entt::registry& reg) {
        const std::string text=m_game.state==result::won?"WIN":"LOSE";
        int columns=-1;for(char c:text)columns+=int(glyph(c)[0].size())+1;
        float x=-float(columns-1)*0.26f;
        for(char c:text) {
            auto rows=glyph(c);
            for(int row=0;row<5;++row) for(std::size_t col=0;col<rows[row].size();++col) if(rows[row][col]=='1') {
                auto entity=spawn(layer,reg,*m_coin.get(),"ResultPixel"+std::to_string(m_banner.size()));
                tint_banner(reg,entity,m_game.state==result::won?m_win_material:m_lose_material);
                m_banner.push_back({entity,{x+float(col)*0.52f,-1.8f+float(row)*0.52f}});
            }
            x+=float(rows[0].size()+1)*0.52f;
        }
    }
    res::res_handle<rnd::geometry_desc> m_fire_geometry;
    res::res_handle<scn::material_desc> m_fire_material;
    entt::entity m_fire_entity=entt::null;
    bool m_preview=false,m_preview_down=false;
    snake_rules m_game;
    std::deque<cell> m_previous;
    std::vector<entt::entity> m_segments;
    std::vector<std::pair<entt::entity,glm::vec2>> m_banner;
    entt::entity m_food_entity=entt::null;
    res::res_handle<scn::prefab_desc> m_character,m_coin;
    res::res_handle<scn::material_desc> m_win_material,m_lose_material;
    float m_time=0.f,m_accumulator=0.f,m_delay=2.f;
    static constexpr float m_tick=0.23f;
    unsigned m_food_serial=1;
    bool m_requested=false,m_initialized=false,m_restart_down=false;
};
class project_game_module final:public gs::game_module {
public:std::string_view name()const override{return SNK_GAME_MODULE_NAME;}
protected:void register_content(scn::scene_layer& layer,ds::app_data_storage&)override {
    rules_self_test();
    layer.systems().register_system<&crystal_run::run,crystal_run>("crystal_run::update",{.phase=ecs::phase::update,.run_in_edit=false});
}
};
}
SNK_DECLARE_MODULE(project_game_module)
