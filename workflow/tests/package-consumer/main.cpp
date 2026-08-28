#include <workflow/nodeflow.hpp>

int main()
{
    workflow::GraphBuilder builder("installed-package-consumer");
    tf::Executor executor;
    builder.run(executor);
    return 0;
}
