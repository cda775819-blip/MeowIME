#define RIME_IMPORTS
#include <stdio.h>
#include <windows.h>
#include "rime_api.h"

static void print_commit(RimeSessionId sid)
{
    RIME_STRUCT(RimeCommit, commit);
    if (RimeGetCommit(sid, &commit))
    {
        printf("COMMIT: [%s]\n", commit.text ? commit.text : "");
        RimeFreeCommit(&commit);
    }
}

static void print_context(RimeSessionId sid)
{
    RIME_STRUCT(RimeContext, ctx);
    if (RimeGetContext(sid, &ctx))
    {
        printf("  preedit: [%s] cursor=%d\n",
            ctx.composition.preedit ? ctx.composition.preedit : "",
            ctx.composition.cursor_pos);
        printf("  menu: num=%d sel=%d page=%d last=%d\n",
            ctx.menu.num_candidates,
            ctx.menu.highlighted_candidate_index,
            ctx.menu.page_no,
            ctx.menu.is_last_page);
        for (int i = 0; i < ctx.menu.num_candidates; i++)
        {
            printf("    [%d] %s\n", i + 1,
                ctx.menu.candidates[i].text ? ctx.menu.candidates[i].text : "");
        }
        RimeFreeContext(&ctx);
    }
}

int main()
{
    SetConsoleOutputCP(CP_UTF8);

    RIME_STRUCT(RimeTraits, traits);
    // 相对路径：librime 按当前工作目录解析。在项目根目录下运行即可。
    traits.shared_data_dir = "rime_data";
    traits.user_data_dir = "rime_user";
    traits.app_name = "rime.cat-test";
    traits.distribution_name = "CatIM";
    traits.distribution_code_name = "cat";
    traits.distribution_version = "0.1";
    traits.log_dir = "rime_user";

    RimeSetup(&traits);
    RimeInitialize(&traits);

    if (RimeStartMaintenance(True))
    {
        RimeJoinMaintenanceThread();
    }

    RimeSessionId sid = RimeCreateSession();
    printf("session=%zu\n", (size_t)sid);

    RimeSchemaList list = {0};
    if (RimeGetSchemaList(&list))
    {
        for (size_t i = 0; i < list.size; i++)
        {
            printf("schema: %s = %s\n", list.list[i].schema_id, list.list[i].name);
        }
        RimeFreeSchemaList(&list);
    }

    if (!RimeSelectSchema(sid, "luna_pinyin"))
    {
        printf("SELECT SCHEMA FAILED\n");
    }

    printf("\n=== type 'nihao' ===\n");
    RimeSimulateKeySequence(sid, "nihao");
    print_commit(sid);
    print_context(sid);

    printf("\n=== select candidate 0 ===\n");
    RimeSelectCandidateOnCurrentPage(sid, 0);
    print_commit(sid);
    print_context(sid);

    printf("\n=== enable zh_hans, type 'nihao' again ===\n");
    RimeSetOption(sid, "zh_hans", True);
    RimeSimulateKeySequence(sid, "nihao");
    print_commit(sid);
    print_context(sid);

    printf("\n=== select candidate 0 (simplified) ===\n");
    RimeSelectCandidateOnCurrentPage(sid, 0);
    print_commit(sid);
    print_context(sid);

    RimeDestroySession(sid);
    RimeFinalize();
    printf("DONE\n");
    return 0;
}
