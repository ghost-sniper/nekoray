package grpc_server

import (
	"context"
	"grpc_server/gen"
)

func (s *BaseServer) Update(_ context.Context, _ *gen.UpdateReq) (*gen.UpdateResp, error) {
	return &gen.UpdateResp{Error: "Use the Nekoray update center to check for updates."}, nil
}
