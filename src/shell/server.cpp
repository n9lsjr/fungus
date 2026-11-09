// Thin gRPC shell: implements snake_sim.RemoteSnake and forwards every
// step to the C brain (brain.h). Keeps per-step work to protobuf parsing
// plus one brain_decide() call.
#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sim_interface.grpc.pb.h"

extern "C" {
#include "brain.h"
}

using grpc::ServerContext;
using grpc::Status;
using snake_sim::Empty;

class SnakeService final : public snake_sim::RemoteSnake::Service {
public:
    Status SetId(ServerContext *, const snake_sim::SnakeId *req, Empty *) override {
        std::lock_guard<std::mutex> lk(mu_);
        id_ = req->id();
        return Status::OK;
    }

    Status SetStartLength(ServerContext *, const snake_sim::StartLength *req, Empty *) override {
        std::lock_guard<std::mutex> lk(mu_);
        start_length_ = req->length();
        return Status::OK;
    }

    Status SetStartPosition(ServerContext *, const snake_sim::StartPosition *, Empty *) override {
        return Status::OK; // start positions also arrive in SetInitData
    }

    Status SetInitData(ServerContext *, const snake_sim::EnvInitData *req, Empty *) override {
        std::lock_guard<std::mutex> lk(mu_);
        brain_init_t init{};
        init.width = req->width();
        init.height = req->height();
        init.free_value = req->free_value();
        init.blocked_value = req->blocked_value();
        init.food_value = req->food_value();
        init.self_id = id_;
        init.start_length = start_length_;

        // Fixed snake order (sorted ids) shared by init and every step.
        ids_.clear();
        for (const auto &kv : req->snake_values()) ids_.push_back(kv.first);
        std::sort(ids_.begin(), ids_.end());
        if (ids_.size() > BRAIN_MAX_SNAKES) ids_.resize(BRAIN_MAX_SNAKES);
        init.n_snakes = static_cast<int>(ids_.size());
        for (int i = 0; i < init.n_snakes; i++) {
            int sid = ids_[i];
            const auto &v = req->snake_values().at(sid);
            init.snake_ids[i] = sid;
            init.head_values[i] = v.head_value();
            init.body_values[i] = v.body_value();
            auto it = req->start_positions().find(sid);
            if (it != req->start_positions().end()) {
                init.start_x[i] = it->second.x();
                init.start_y[i] = it->second.y();
            }
        }
        base_map_.assign(req->base_map().begin(), req->base_map().end());
        init.base_map = base_map_.size() == size_t(init.width * init.height)
                            ? base_map_.data() : nullptr;
        brain_init(&init);
        return Status::OK;
    }

    Status Update(ServerContext *,
                  grpc::ServerReaderWriter<snake_sim::UpdateResponse, snake_sim::EnvData> *stream) override {
        snake_sim::EnvData req;
        snake_sim::UpdateResponse resp;
        brain_step_t step;
        while (stream->Read(&req)) {
            std::memset(&step, 0, sizeof step);
            step.map = reinterpret_cast<const uint8_t *>(req.map().data());
            step.n_snakes = static_cast<int>(ids_.size());
            for (int i = 0; i < step.n_snakes; i++) {
                auto it = req.snakes().find(ids_[i]);
                if (it == req.snakes().end()) continue;
                step.alive[i] = it->second.is_alive() ? 1 : 0;
                step.lengths[i] = it->second.length();
            }
            int nf = std::min(req.food_locations_size(), BRAIN_MAX_FOOD);
            step.n_food = nf;
            for (int i = 0; i < nf; i++) {
                step.food_x[i] = static_cast<int16_t>(req.food_locations(i).x());
                step.food_y[i] = static_cast<int16_t>(req.food_locations(i).y());
            }
            int dx = 0, dy = 0;
            resp.Clear();
            if (brain_decide(&step, &dx, &dy) == 0) {
                resp.mutable_direction()->set_x(dx);
                resp.mutable_direction()->set_y(dy);
            }
            if (!stream->Write(resp)) break;
        }
        return Status::OK;
    }

    Status Reset(ServerContext *, const Empty *, Empty *) override { return Status::OK; }
    Status Kill(ServerContext *, const Empty *, Empty *) override { return Status::OK; }

private:
    std::mutex mu_;
    int id_ = -1;
    int start_length_ = 1;
    std::vector<int> ids_;
    std::vector<uint8_t> base_map_;
};

int main(int argc, char **argv) {
    std::string addr = "0.0.0.0:50051";
    if (argc > 1) addr = argv[1];
    else if (const char *e = std::getenv("CSNAKE_ADDR")) addr = e;

    SnakeService service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    // One sim connection per match; a small pool keeps idle CPU near zero.
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::NUM_CQS, 1);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MIN_POLLERS, 1);
    builder.SetSyncServerOption(grpc::ServerBuilder::SyncServerOption::MAX_POLLERS, 2);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "failed to listen on %s\n", addr.c_str());
        return 1;
    }
    std::fprintf(stderr, "csnake listening on %s\n", addr.c_str());
    server->Wait();
    return 0;
}
