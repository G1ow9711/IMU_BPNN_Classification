"""验证 Agents.md 中文注释审计器对语义块和数值数组的边界判断。"""

# 导入 unittest，使用标准库执行无第三方依赖的确定性测试。
import unittest
# 导入 JSON，构造与字体生成器一致的测试清单。
import json
# 导入临时目录，保证破坏性边界测试不会修改仓库真实字体。
import tempfile
# 导入 Path，为被审计的虚拟路径提供与生产函数相同的参数类型。
from pathlib import Path
# 导入 mock.patch，隔离 Git 查询并验证两种审计模式使用的精确命令。
from unittest.mock import patch

# 导入待测试的连续语义块审计函数和门槛常量。
from check_agents_compliance import (
    GENERATED_FEATURE_HEADER,
    GENERATED_MODEL_HEADER,
    GENERATED_UI_FONT_LICENSE,
    GENERATED_UI_FONT_MANIFEST,
    MAX_UNCOMMENTED_SEMANTIC_LINES,
    audit_enum_member_comments,
    audit_long_parameter_comments,
    audit_public_array_pointer_contracts,
    audit_uncommented_semantic_blocks,
    build_argument_parser,
    collect_all_tracked_sources,
    collect_changed_sources,
    main,
    sha256_file,
    validate_generated_ui_fonts,
)


class SourceCollectionModeTests(unittest.TestCase):
    """覆盖默认差异模式、--all 全跟踪模式和生成物专用合同入口。"""

    def _write_source(self, repository_root: Path, relative_path: Path) -> None:
        """创建最小中文源码或合同工件，使收集器能验证路径而不依赖真实仓库。"""

        # absolute_path 把仓库相对路径绑定到当前临时目录。
        absolute_path = repository_root / relative_path
        # 创建父目录，兼容模型头和字体清单的深层固定路径。
        absolute_path.parent.mkdir(parents=True, exist_ok=True)
        # 写入最小中文内容；收集测试只验证路径，不执行文件内容合规审计。
        absolute_path.write_text("/* 测试跟踪文件。 */\n", encoding="utf-8")

    def test_argument_parser_defaults_to_changed_mode(self) -> None:
        """未提供参数时必须保持历史差异模式，显式 --all 才切换全仓库。"""

        # parser 使用生产中文帮助与参数定义，避免测试复制另一份 CLI 合同。
        parser = build_argument_parser()
        # 空参数列表代表日常提交前调用，audit_all 必须为 False。
        self.assertFalse(parser.parse_args([]).audit_all)
        # 显式 --all 代表开源发布前全量审计，audit_all 必须为 True。
        self.assertTrue(parser.parse_args(["--all"]).audit_all)
        # 帮助文本必须同时说明默认范围、全跟踪范围和 git ls-files 数据来源。
        help_text = parser.format_help()
        self.assertIn("默认只审计", help_text)
        self.assertIn("git ls-files", help_text)
        self.assertIn("显示此帮助信息并退出", help_text)

    def test_changed_mode_keeps_tracked_and_untracked_sources(self) -> None:
        """默认模式必须继续合并 HEAD 差异和未跟踪源码，不能因新增 --all 缩窄。"""

        # 临时目录提供两个真实存在的 Python 文件，满足统一过滤器的 is_file 条件。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 模拟仓库根。
            repository_root = Path(temporary_directory)
            # tracked_path 模拟相对 HEAD 修改的已跟踪源码。
            tracked_path = Path("python/tracked_change.py")
            # untracked_path 模拟尚未 git add 的新源码。
            untracked_path = Path("python/new_module.py")
            # 创建两个候选文件。
            self._write_source(repository_root, tracked_path)
            self._write_source(repository_root, untracked_path)
            # side_effect 按生产调用顺序返回 diff 路径和未跟踪路径。
            with patch(
                "check_agents_compliance.run_git_lines",
                side_effect=[[tracked_path.as_posix()], [untracked_path.as_posix()]],
            ) as git_query:
                # 执行默认差异收集器。
                actual_paths = collect_changed_sources(repository_root)
            # 两类路径必须全部保留且按稳定顺序返回。
            self.assertEqual(
                sorted([tracked_path, untracked_path], key=lambda path: path.as_posix().lower()),
                actual_paths,
            )
            # 第一条查询必须继续使用相对 HEAD 差异。
            self.assertEqual(
                ["diff", "--name-only", "--diff-filter=ACMR", "HEAD"],
                git_query.call_args_list[0].args[1],
            )
            # 第二条查询必须继续包含未跟踪且未忽略文件。
            self.assertEqual(
                ["ls-files", "--others", "--exclude-standard"],
                git_query.call_args_list[1].args[1],
            )

    def test_all_mode_uses_only_git_tracked_sources_and_keeps_generated_contracts(self) -> None:
        """--all 必须只用 git ls-files，并保留模型、特征和字体专用合同触发路径。"""

        # 临时目录隔离全量文件集合，避免真实工作树未跟踪文件影响断言。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 模拟仓库根。
            repository_root = Path(temporary_directory)
            # tracked_source 是普通已跟踪自研源码。
            tracked_source = Path("python/train_export.py")
            # generated_paths 覆盖模型头、特征头、字体清单和许可证四类专用入口。
            generated_paths = [
                GENERATED_MODEL_HEADER,
                GENERATED_FEATURE_HEADER,
                GENERATED_UI_FONT_MANIFEST,
                GENERATED_UI_FONT_LICENSE,
            ]
            # documentation_path 是已跟踪 Markdown，必须被源码过滤器排除。
            documentation_path = Path("docs/README.md")
            # missing_source 模拟索引存在但工作树缺失的路径，不得送入文本读取。
            missing_source = Path("python/deleted_locally.py")
            # 创建普通源码和全部生成合同入口。
            self._write_source(repository_root, tracked_source)
            for generated_path in generated_paths:
                self._write_source(repository_root, generated_path)
            # Markdown 虽被 git ls-files 返回，但不属于本审计器源码范围。
            self._write_source(repository_root, documentation_path)
            # git_paths 精确模拟索引内容；missing_source 故意不创建。
            git_paths = [
                tracked_source.as_posix(),
                *(path.as_posix() for path in generated_paths),
                documentation_path.as_posix(),
                missing_source.as_posix(),
            ]
            # mock 保证测试只验证 `git ls-files` 合同，不依赖系统 Git。
            with patch(
                "check_agents_compliance.run_git_lines",
                return_value=git_paths,
            ) as git_query:
                # 执行全跟踪源码收集器。
                actual_paths = collect_all_tracked_sources(repository_root)
            # 全量模式必须只执行一次不带其它筛选参数的 git ls-files。
            git_query.assert_called_once_with(repository_root, ["ls-files"])
            # 普通源码和四类生成合同入口必须进入主审计循环。
            expected_paths = sorted(
                [tracked_source, *generated_paths],
                key=lambda path: path.as_posix().lower(),
            )
            self.assertEqual(expected_paths, actual_paths)
            # Markdown 和工作树缺失文件必须被排除，防止读取异常或文档误报。
            self.assertNotIn(documentation_path, actual_paths)
            self.assertNotIn(missing_source, actual_paths)

    def test_main_without_all_dispatches_only_changed_collector(self) -> None:
        """main 空参数必须只调用差异收集器，防止日常审计意外变成昂贵全仓扫描。"""

        # 差异收集器返回空集合，使 main 无需读取真实源码即可完成确定性分派测试。
        with patch(
            "check_agents_compliance.collect_changed_sources",
            return_value=[],
        ) as changed_collector:
            # 全量收集器若被误调用会被断言捕获。
            with patch(
                "check_agents_compliance.collect_all_tracked_sources",
                return_value=[],
            ) as all_collector:
                # 屏蔽成功标记输出，保持单元测试日志聚焦断言。
                with patch("builtins.print"):
                    # 空参数列表模拟默认命令行调用。
                    return_code = main([])
        # 无源码变更仍应返回成功。
        self.assertEqual(0, return_code)
        # 默认模式必须且只能调用一次差异收集器。
        changed_collector.assert_called_once()
        # 默认模式不得读取全部 Git 跟踪文件。
        all_collector.assert_not_called()

    def test_main_all_dispatches_generated_specialized_contracts(self) -> None:
        """--all 主流程必须执行模型、特征和中文字体三类专用完整性合同。"""

        # full_paths 用最少三个入口覆盖主循环的三条生成物专用分支。
        full_paths = [
            GENERATED_MODEL_HEADER,
            GENERATED_FEATURE_HEADER,
            GENERATED_UI_FONT_MANIFEST,
        ]
        # 全量收集器返回专用入口，避免测试依赖真实 Git 状态。
        with patch(
            "check_agents_compliance.collect_all_tracked_sources",
            return_value=full_paths,
        ) as all_collector:
            # 差异收集器在 --all 模式绝不能被调用。
            with patch(
                "check_agents_compliance.collect_changed_sources",
                return_value=[],
            ) as changed_collector:
                # 模型头验证器返回通过并记录调用。
                with patch(
                    "check_agents_compliance.validate_generated_header",
                    return_value=[],
                ) as model_validator:
                    # 特征头验证器返回通过并记录调用。
                    with patch(
                        "check_agents_compliance.validate_generated_feature_header",
                        return_value=[],
                    ) as feature_validator:
                        # 字体包验证器返回通过并记录调用。
                        with patch(
                            "check_agents_compliance.validate_generated_ui_fonts",
                            return_value=[],
                        ) as font_validator:
                            # 普通特征头中文审计返回通过，避免读取测试夹具外文件。
                            with patch(
                                "check_agents_compliance.audit_source_file",
                                return_value=[],
                            ):
                                # 屏蔽成功标记输出。
                                with patch("builtins.print"):
                                    # 显式 --all 启动全仓库主流程。
                                    return_code = main(["--all"])
        # 三类专用合同全部通过时主流程返回成功。
        self.assertEqual(0, return_code)
        # --all 必须调用全跟踪收集器一次。
        all_collector.assert_called_once()
        # --all 不得混入默认差异收集器。
        changed_collector.assert_not_called()
        # 模型、特征和字体专用验证器都必须执行一次。
        model_validator.assert_called_once()
        feature_validator.assert_called_once()
        font_validator.assert_called_once()


class SemanticBlockAuditTests(unittest.TestCase):
    """覆盖超长漏注释块、中文分隔和数值初始化例外。"""

    def test_reports_block_longer_than_limit(self) -> None:
        """连续语义行超过门槛时应报告起止位置。"""

        # statements 生成门槛加一条独立 C 赋值，模拟整段函数缺少中文说明。
        statements = "\n".join(
            # 每行以分号结束，明确属于独立语义语句。
            f"value_{index} = {index};"
            # 范围多一项，确保刚好越过门槛。
            for index in range(MAX_UNCOMMENTED_SEMANTIC_LINES + 1)
        )
        # 调用生产审计器并绑定可读的虚拟 C 文件名。
        errors = audit_uncommented_semantic_blocks(statements, Path("sample.c"))
        # 超长块必须产生且只产生一个聚合错误，避免后续每行重复刷屏。
        self.assertEqual(1, len(errors))
        # 错误应包含首行，便于开发者直接跳转。
        self.assertIn("lines=1-", errors[0])

    def test_chinese_comment_resets_consecutive_count(self) -> None:
        """每个短代码组前有中文说明时不应跨组累计。"""

        # source 把两组短赋值用中文说明分隔，总语义行数虽多但每块都在门槛内。
        source = "\n".join(
            ["/* 第一组变量赋值。 */"]
            # 第一组占半个门槛。
            + [f"left_{index} = {index};" for index in range(MAX_UNCOMMENTED_SEMANTIC_LINES // 2)]
            # 中文说明必须把连续计数清零。
            + ["/* 第二组变量赋值。 */"]
            # 第二组也只占半个门槛。
            + [f"right_{index} = {index};" for index in range(MAX_UNCOMMENTED_SEMANTIC_LINES // 2)]
        )
        # 审计结果应为空，说明注释作用域没有错误跨越。
        self.assertEqual([], audit_uncommented_semantic_blocks(source, Path("sample.c")))

    def test_numeric_initializer_and_format_braces_are_ignored(self) -> None:
        """带中文形状说明的长数值数组不应要求逐元素注释。"""

        # numeric_lines 模拟自动生成权重数组的多行 float 字面量。
        numeric_lines = ["    0.1f, 0.2f, 0.3f,"] * (MAX_UNCOMMENTED_SEMANTIC_LINES + 10)
        # source 在数组前说明形状和用途，数组元素及闭合花括号属于允许例外。
        source = "\n".join(
            [
                "/* 权重数组形状 [64,80]，按输出优先行主序保存。 */",
                "static const float WEIGHTS[5120] = {",
                *numeric_lines,
                "};",
            ]
        )
        # 数值初始化区不应形成超长语义块。
        self.assertEqual([], audit_uncommented_semantic_blocks(source, Path("weights.h")))


class DetailedCommentContractTests(unittest.TestCase):
    """覆盖枚举、长参数表、数组形状和指针生命周期的严格中文合同。"""

    def test_enum_member_requires_individual_chinese_semantics(self) -> None:
        """只有枚举总述而没有逐成员说明时必须失败。"""

        # source 模拟仅有一行总述、成员本身没有中文语义的 C 枚举。
        source = """/* 描述电源事件。 */
typedef enum {
    POWER_EVENT_START = 0,
    POWER_EVENT_STOP,
    POWER_EVENT_SHUTDOWN
} power_event_t;
"""
        # 三个成员都没有同一行或紧邻上一行的中文说明，审计必须报告。
        errors = audit_enum_member_comments(source, Path("power_manager.h"))
        # 错误中必须包含第二个成员，证明不是只检查枚举首项。
        self.assertTrue(any("POWER_EVENT_STOP" in error for error in errors))

    def test_enum_member_accepts_adjacent_chinese_comment(self) -> None:
        """每个枚举成员前有专属中文说明时应通过。"""

        # source 为每个成员提供紧邻中文说明，语义不会错误复用到下一成员。
        source = """typedef enum {
    /* 开始训练。 */
    POWER_EVENT_START = 0,
    /* 停止并保存训练。 */
    POWER_EVENT_STOP,
    /* 保存完成后安全关机。 */
    POWER_EVENT_SHUTDOWN
} power_event_t;
"""
        # 完整逐项说明不得产生误报。
        self.assertEqual([], audit_enum_member_comments(source, Path("power_manager.h")))

    def test_long_parameter_list_requires_per_item_chinese_comment(self) -> None:
        """八项以上公开参数表不能只依赖函数总述。"""

        # source 模拟 C# 长构造器；参数行没有逐项中文说明。
        source = """/// <summary>创建设备诊断快照。</summary>
public DeviceSnapshot(
    string deviceId,
    bool connected,
    string model,
    string revision,
    int? rssi,
    ushort? mtu,
    byte? battery,
    uint errors)
{
}
"""
        # 长参数审计必须指出至少一个未说明参数。
        errors = audit_long_parameter_comments(source, Path("DeviceSnapshot.cs"))
        # deviceId 位于首项，也必须拥有自己的中文解释。
        self.assertTrue(any("deviceId" in error for error in errors))

    def test_public_array_pointer_contract_requires_shape_unit_and_lifetime(self) -> None:
        """公开数组与指针 API 必须声明形状、单位、可空性和生命周期。"""

        # source 只给模糊总述，没有数组形状、物理单位和指针借用期。
        source = """/* 执行模型推理。 */
int infer(void *context, const float window[62][6], float logits[11]);
"""
        # 高风险公开数组/指针合同必须被拒绝。
        errors = audit_public_array_pointer_contracts(source, Path("imu_pipeline_dual_m0.h"))
        # 错误应同时要求形状和生命周期信息，而不是只统计中文比例。
        self.assertTrue(any("形状" in error for error in errors))
        self.assertTrue(any("生命周期" in error for error in errors))


class GeneratedUiFontAuditTests(unittest.TestCase):
    """覆盖中文字体生成包的有效路径、缺字、摘要、规格、许可证和文件缺失。"""

    # 四个固定字号与生产 renderer 使用的 lv_font_t 名称必须一一对应。
    FONT_SPECS = (
        (16, "ui_font_noto_sans_sc_16"),
        (20, "ui_font_noto_sans_sc_20"),
        (28, "ui_font_noto_sans_sc_28"),
        (36, "ui_font_noto_sans_sc_36"),
    )
    # 生成器固定加入九个常用全角标点；有效夹具必须完整声明这些字符。
    REQUIRED_PUNCTUATION = "，。！？：；（）、"

    def _create_valid_font_bundle(self, repository_root: Path) -> dict[str, object]:
        """在临时仓库创建最小有效字体包，并返回可供边界测试修改的清单对象。"""

        # presenter_directory 对应生产 ui_presenter.c 的固定相对目录。
        presenter_directory = repository_root / "esp32/firmware/components/ui"
        # fonts_directory 保存三字号、清单和许可证，与生产路径完全一致。
        fonts_directory = presenter_directory / "fonts"
        # 创建全部父目录，临时目录初始为空。
        fonts_directory.mkdir(parents=True, exist_ok=True)
        # presenter 只含真实用户字符串“启动”；中文注释“忽略”不应进入字库合同。
        (presenter_directory / "ui_presenter.c").write_text(
            '/* 这段中文注释应被忽略。 */\nconst char *label = "启动";\n',
            encoding="utf-8",
        )
        # 许可证包含审计器要求的官方名称和版本，模拟仓库完整 OFL 文本入口。
        (fonts_directory / "OFL-1.1.txt").write_text(
            "Copyright Test Font Author\nSIL OPEN FONT LICENSE Version 1.1\n",
            encoding="utf-8",
        )
        # symbols 合并 presenter 两个汉字和生成器固定标点，形成完整覆盖集合。
        symbols = "启动" + self.REQUIRED_PUNCTUATION
        # generated_fonts 按生产顺序保存三个字号记录。
        generated_fonts: list[dict[str, object]] = []
        # 遍历三个固定规格，构造含中文生成头、symbols 和导出符号的最小 C 文件。
        for size_px, font_name in self.FONT_SPECS:
            # relative_path 使用 manifest 规定的 POSIX 仓库相对路径。
            relative_path = Path(
                f"esp32/firmware/components/ui/fonts/{font_name}.c"
            )
            # font_path 指向当前临时仓库内的实际文件。
            font_path = repository_root / relative_path
            # font_text 保留审计器要求的三个中文标记、字符集合和 lv_font_t 符号。
            font_text = (
                f"/* 自动生成中文界面字体：{size_px}px、2 bpp 非压缩位图；"
                "禁止手工调整数组；许可见 OFL-1.1.txt。 */\n"
                f"/* --symbols {symbols} */\n"
                "static const lv_font_fmt_txt_dsc_t font_dsc = { .bitmap_format = 0 };\n"
                f"const lv_font_t {font_name} = {{0}};\n"
            )
            # 使用 UTF-8 写入，摘要按同一最终字节计算。
            font_path.write_text(font_text, encoding="utf-8")
            # 将当前字号的固定规格、长度和真实 SHA-256 加入清单。
            generated_fonts.append(
                {
                    "name": font_name,
                    "size_px": size_px,
                    "bpp": 2,
                    "file": relative_path.as_posix(),
                    "bytes": font_path.stat().st_size,
                    "sha256": sha256_file(font_path),
                }
            )
        # manifest 包含审计器使用的授权、字符和三个生成物字段。
        manifest: dict[str, object] = {
            "schema_version": 1,
            "license": "SIL Open Font License 1.1",
            "license_file": "esp32/firmware/components/ui/fonts/OFL-1.1.txt",
            "chinese_glyph_count": len(set(symbols)),
            "chinese_symbols": symbols,
            "generated_fonts": generated_fonts,
        }
        # 把 JSON 以 UTF-8 中文可读形式写入固定清单路径。
        (fonts_directory / "ui_font_manifest.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        # 返回清单对象，调用测试可修改后重写以制造边界错误。
        return manifest

    def _write_manifest(self, repository_root: Path, manifest: dict[str, object]) -> None:
        """把修改后的清单写回固定路径，避免各测试重复 JSON 样板。"""

        # manifest_path 与生产验证器读取位置完全一致。
        manifest_path = (
            repository_root
            / "esp32/firmware/components/ui/fonts/ui_font_manifest.json"
        )
        # 保留中文原字符，便于失败信息和测试夹具人工检查。
        manifest_path.write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )

    def test_valid_generated_ui_font_bundle_passes(self) -> None:
        """三字号、2 bpp、授权、字符和摘要完整时应通过。"""

        # 临时目录保证有效夹具不会依赖仓库当前未跟踪文件状态。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 是生产验证器所需的仓库根参数。
            repository_root = Path(temporary_directory)
            # 创建完整有效包。
            self._create_valid_font_bundle(repository_root)
            # 有效生成证据不得产生任何合规错误。
            self.assertEqual([], validate_generated_ui_fonts(repository_root))

    def test_presenter_missing_glyph_is_rejected(self) -> None:
        """presenter 新增汉字但未重生成字体时必须报告缺字。"""

        # 临时目录隔离 presenter 破坏性修改。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 绑定当前测试夹具。
            repository_root = Path(temporary_directory)
            # 先建立只有“启动”字形的有效字体包。
            self._create_valid_font_bundle(repository_root)
            # 把用户文案改为“启动训练”，但故意不更新 symbols 和位图。
            presenter_path = repository_root / "esp32/firmware/components/ui/ui_presenter.c"
            # 新增“训”“练”应触发实际文案覆盖检查。
            presenter_path.write_text(
                'const char *label = "启动训练";\n',
                encoding="utf-8",
            )
            # 执行整包验证并收集缺字错误。
            errors = validate_generated_ui_fonts(repository_root)
            # 错误必须明确来自 ui_presenter.c 字符覆盖，而非模糊哈希问题。
            self.assertTrue(any("ui_presenter.c 字符" in error for error in errors))
            # 两个新增汉字都应出现在错误文本中，方便开发者直接重生成。
            self.assertTrue(any("训" in error and "练" in error for error in errors))

    def test_font_hash_mismatch_is_rejected(self) -> None:
        """生成 C 文件在清单写出后被修改时必须拒绝。"""

        # 临时目录隔离对字体文件的手工篡改。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 作为验证器工作根。
            repository_root = Path(temporary_directory)
            # 先创建哈希一致的有效包。
            self._create_valid_font_bundle(repository_root)
            # font_path 选择 16px 文件，模拟生成后手工追加内容。
            font_path = repository_root / "esp32/firmware/components/ui/fonts/ui_font_noto_sans_sc_16.c"
            # 追加字节会同时改变长度和 SHA-256，但不破坏生成头标记。
            with font_path.open("a", encoding="utf-8") as stream:
                # 追加英文标记，确保失败只由完整性字段触发。
                stream.write("/* tampered */\n")
            # 执行验证并确认摘要错误被报告。
            errors = validate_generated_ui_fonts(repository_root)
            # SHA-256 是禁止手工修改大型位图的核心证据，必须明确失败。
            self.assertTrue(any("SHA-256" in error for error in errors))

    def test_compressed_font_bitmap_is_rejected(self) -> None:
        """真板出现重影后，任何 bitmap_format=1 压缩中文字体都必须拒绝。"""

        # 临时目录隔离压缩格式篡改。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 指向完整有效字体包。
            repository_root = Path(temporary_directory)
            # manifest 返回可更新摘要的完整对象。
            manifest = self._create_valid_font_bundle(repository_root)
            # font_path 选择 20px 字体模拟 lv_font_conv 默认压缩输出。
            font_path = repository_root / "esp32/firmware/components/ui/fonts/ui_font_noto_sans_sc_20.c"
            # 把唯一非压缩标记替换为压缩格式 1。
            font_text = font_path.read_text(encoding="utf-8").replace(
                ".bitmap_format = 0",
                ".bitmap_format = 1",
            )
            # 写回压缩模拟文件。
            font_path.write_text(font_text, encoding="utf-8")
            # 更新 bytes 和摘要，使失败只来自格式合同而非完整性检查。
            records = manifest["generated_fonts"]
            # 20px 是固定列表第二项。
            records[1]["bytes"] = font_path.stat().st_size  # type: ignore[index]
            # 重算 SHA-256，避免摘要错误掩盖压缩格式错误。
            records[1]["sha256"] = sha256_file(font_path)  # type: ignore[index]
            # 写回同步清单。
            self._write_manifest(repository_root, manifest)
            # 执行生产审计器。
            errors = validate_generated_ui_fonts(repository_root)
            # 错误必须明确指出 bitmap_format=0，而不是模糊视觉描述。
            self.assertTrue(any("bitmap_format=0" in error for error in errors))

    def test_wrong_size_and_bpp_are_rejected(self) -> None:
        """字号集合不是16/20/28/36或任一位深不是2时必须拒绝。"""

        # 临时目录隔离清单规格修改。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 指向测试仓库根。
            repository_root = Path(temporary_directory)
            # manifest 返回可修改的完整对象。
            manifest = self._create_valid_font_bundle(repository_root)
            # records 是三个字体清单对象；夹具保证这里为列表。
            records = manifest["generated_fonts"]
            # 把 20px 位深改为 4，验证视觉/Flash 取舍不能静默变化。
            records[1]["bpp"] = 4  # type: ignore[index]
            # 把 28px 字号改成 30，破坏固定字号集合。
            records[2]["size_px"] = 30  # type: ignore[index]
            # 写回篡改后的清单。
            self._write_manifest(repository_root, manifest)
            # 执行验证并收集规格错误。
            errors = validate_generated_ui_fonts(repository_root)
            # 位深错误必须明确指出 20px 与 2 bpp 合同。
            self.assertTrue(any("20px" in error and "bpp" in error for error in errors))
            # 字号集合错误必须包含固定 16/20/28/36 文案。
            self.assertTrue(any("16/20/28/36px" in error for error in errors))

    def test_missing_license_and_font_file_are_rejected(self) -> None:
        """许可证或任一字号文件缺失时生成例外不得通过。"""

        # 临时目录隔离删除操作。
        with tempfile.TemporaryDirectory() as temporary_directory:
            # repository_root 指向完整测试包根。
            repository_root = Path(temporary_directory)
            # 先创建有效包，确保删除前没有其它错误。
            self._create_valid_font_bundle(repository_root)
            # 删除授权文本，模拟打包时漏带 OFL 文件。
            (repository_root / "esp32/firmware/components/ui/fonts/OFL-1.1.txt").unlink()
            # 删除 28px 字体，模拟 CMake 引用工件未生成。
            (
                repository_root
                / "esp32/firmware/components/ui/fonts/ui_font_noto_sans_sc_28.c"
            ).unlink()
            # 执行整包验证。
            errors = validate_generated_ui_fonts(repository_root)
            # 许可证缺失必须阻止可分发构建。
            self.assertTrue(any("许可证缺失" in error for error in errors))
            # 字体文件缺失必须带具体路径或字号。
            self.assertTrue(any("字体文件缺失" in error and "28" in error for error in errors))


# 直接执行本文件时启动标准 unittest；被发现器导入时不重复运行。
if __name__ == "__main__":
    # unittest.main 根据断言结果返回可供 PowerShell/CI 判定的退出码。
    unittest.main()
