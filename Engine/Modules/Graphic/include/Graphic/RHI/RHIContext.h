#pragma once
#include <memory>
#include <Graphic/API.h>
#include <Graphic/RHI/RHIBackend.h>

namespace Oyi::Graphic
{
    class OYI_GRAPHIC_API RHIContext
    {
    public:
        virtual ~RHIContext() = default;

        virtual BackendType backend() const = 0;

        virtual void initialize() = 0;
        virtual void shutdown() = 0;
    };

    using RHIContextPtr = std::unique_ptr<RHIContext>;
}
