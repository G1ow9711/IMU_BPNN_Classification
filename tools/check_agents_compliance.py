"""审计本次新增或修改源码是否满足 Agents.md 的中文注释合同。"""

# 导入命令行参数模块，用于提供默认差异审计和显式全仓库审计两种入口。
import argparse
# 导入哈希模块，用于确认自动生成模型头没有在生成后被手工修改。
import hashlib
# 导入 JSON 模块，用于读取双 M0 导出清单中的权威文件摘要。
import json
# 导入正则表达式模块，用于识别中文字符和自动生成数组声明。
import re
# 导入子进程模块，用于向 Git 查询本次变更文件集合。
import subprocess
# 导入系统模块，用于向调用方返回可用于 CI 判定的退出码。
import sys
# 导入 Path，统一处理 Windows 与 POSIX 路径分隔符。
from pathlib import Path


# 这些扩展名属于本项目自研代码或界面声明，需要执行中文注释密度检查。
SOURCE_SUFFIXES = {
    ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".ino",
    ".cs", ".py", ".ps1", ".xaml",
}
# CMakeLists.txt 没有扩展名，但会改变组件依赖和固件构建结果，因此单独纳入。
SPECIAL_SOURCE_NAMES = {"CMakeLists.txt"}
# 连续语义块启发式只用于分号/花括号明确的 C、C++ 头和 C#；Python/XAML/PowerShell 由密度检查覆盖。
SEMANTIC_BLOCK_SUFFIXES = {".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".ino", ".cs"}
# 超过该非空行数的文件使用注释密度门槛，避免只有文件头一句中文的形式化注释。
LARGE_FILE_NONBLANK_THRESHOLD = 20
# 中文行比例至少为 20%；逐行说明通常会明显高于该最低启发式门槛。
MIN_CHINESE_LINE_RATIO = 0.20
# 连续语义代码超过二十四行仍没有中文说明时视为整块漏注释；阈值允许参数表、测试断言组和声明组。
MAX_UNCOMMENTED_SEMANTIC_LINES = 24
# 自动生成权重头含数千项数值，逐元素注释会破坏可读性和生成哈希，因此允许唯一例外。
GENERATED_MODEL_HEADER = Path("esp32/include/esp32_dual_m0_model.h")
# 自动生成特征头仍执行普通密度和语义块审计，并额外通过清单哈希证明来自官方导出器。
GENERATED_FEATURE_HEADER = Path("esp32/include/esp32_bp_features.h")
# 生成清单保存三个部署工件的 SHA-256，是自动生成例外的完整性依据。
GENERATED_MANIFEST = Path("esp32/include/dual_m0_manifest.json")
# 中文字体目录保存四个字号位图、授权文本和生成清单；整个目录按一个生成包审计。
GENERATED_UI_FONT_DIRECTORY = Path("esp32/firmware/components/ui/fonts")
# 字体清单记录源字体授权、中文字符集合、字号、位深和每个 C 文件摘要。
GENERATED_UI_FONT_MANIFEST = GENERATED_UI_FONT_DIRECTORY / "ui_font_manifest.json"
# 字体许可证必须随固件源码分发，保证 Noto Sans SC 子集满足 SIL OFL 1.1。
GENERATED_UI_FONT_LICENSE = GENERATED_UI_FONT_DIRECTORY / "OFL-1.1.txt"
# 四个字体 C 文件是 lv_font_conv 生成的大型数值位图，使用专用完整性合同替代逐行注释。
GENERATED_UI_FONT_FILES = (
    GENERATED_UI_FONT_DIRECTORY / "ui_font_noto_sans_sc_16.c",
    GENERATED_UI_FONT_DIRECTORY / "ui_font_noto_sans_sc_20.c",
    GENERATED_UI_FONT_DIRECTORY / "ui_font_noto_sans_sc_28.c",
    GENERATED_UI_FONT_DIRECTORY / "ui_font_noto_sans_sc_36.c",
)
# 字体包任一成员变化都触发整包验证，防止只改清单或删除许可证绕过审计。
GENERATED_UI_FONT_AUDIT_PATHS = frozenset(
    (*GENERATED_UI_FONT_FILES, GENERATED_UI_FONT_MANIFEST, GENERATED_UI_FONT_LICENSE)
)
# presenter 是设备用户可见文案的唯一字形来源，注释中的汉字不应扩大固件 Flash。
UI_PRESENTER_SOURCE = Path("esp32/firmware/components/ui/ui_presenter.c")
# 字体生成器固定附带常用全角标点，避免以后增加提示标点却出现方框。
UI_FONT_REQUIRED_PUNCTUATION = frozenset("，。！？：；（）、")
# 字符串字面量模式忽略转义引号，只从真实 C 字符串中提取用户可见中文。
C_STRING_LITERAL_PATTERN = re.compile(r'"(?:[^"\\]|\\.)*"')
# 中文字符范围覆盖常用汉字和扩展 A 区，可识别代码注释中的中文说明。
CHINESE_PATTERN = re.compile(r"[\u3400-\u4dbf\u4e00-\u9fff]")
# 数组声明模式只匹配模型头中的 float 常量，要求每组权重前都存在形状与内存说明。
FLOAT_ARRAY_PATTERN = re.compile(r"^\s*static\s+const\s+float\s+[A-Za-z0-9_]+\s*\[")
# 纯数值初始化行只承载自动生成或查表数据，不要求为每个元素重复写中文注释。
NUMERIC_INITIALIZER_LINE_PATTERN = re.compile(
    r"^[\s,{}()\[\]+\-0-9.eExXa-fA-FfFuUlL]+$"
)
# 纯格式行只负责闭合括号、逗号或预处理分支，不构成独立算法语义。
FORMAT_ONLY_PATTERN = re.compile(r"^[\s{}()\[\],;]+$")
# C/C++ 数值数组声明开启数组数据区；区内纯数值元素由数组声明前的中文形状说明覆盖。
NUMERIC_ARRAY_START_PATTERN = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?(?:float|double|int\w*|uint\w*|size_t)\s+"
    r"[A-Za-z_][A-Za-z0-9_]*\s*\[[^;]*=\s*\{\s*$"
)
# 枚举成员行模式提取 C/C++ 与 C# 的标识符；赋值表达式由逗号或行尾终止。
ENUM_MEMBER_PATTERN = re.compile(
    r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(?:=[^,}]*)?\s*,?\s*(?://.*|/\*.*\*/)?$"
)
# 声明前缀模式覆盖 Python 函数、C# 可见成员/record 以及 C/C++ 头文件原型。
DECLARATION_PREFIX_PATTERN = re.compile(
    r"(?:\bdef\s+[A-Za-z_]\w*|"
    r"\b(?:public|protected|private|internal|static|virtual|override|async|partial|sealed|record)\b[^=;{}]*[A-Za-z_]\w*|"
    r"\b(?:extern\s+)?(?:const\s+)?(?:void|bool|char|short|int|long|float|double|size_t|u?int\d+_t|[A-Za-z_]\w*_t)"
    r"(?:\s+const)?(?:\s*\*+)?\s+[A-Za-z_]\w*)\s*$"
)
# 控制语句和表达式调用不能当成函数声明，否则长调用参数会产生错误注释告警。
NON_DECLARATION_PREFIX_PATTERN = re.compile(
    r"^\s*(?:if|for|while|switch|catch|return|sizeof|assert|new)\b"
)
# 参数名模式取去掉默认值与数组后缀后的最后一个标识符，兼容 C 指针和 C# 可空类型。
PARAMETER_NAME_PATTERN = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*(?:\[[^\]]*\]\s*)*$")
# 数组合同至少要求显式形状，并说明单位或该数组是无量纲分数。
ARRAY_UNIT_KEYWORDS = ("单位", "无量纲")
# 通用指针合同必须明确借用生命周期与可空性，防止调用者误释放或传入空地址。
POINTER_NULLABILITY_KEYWORDS = ("可为空", "允许为空", "不能为空", "不得为空", "非空")


def _find_matching_parenthesis(text: str, open_index: int) -> int:
    """从左括号开始寻找同层右括号；忽略字符串和注释的轻量解析足以覆盖声明。"""

    # depth 记录圆括号嵌套层级，支持函数指针和默认表达式中的括号。
    depth = 0
    # quote 记录当前字符串引号；None 表示不在普通字符串中。
    quote: str | None = None
    # escaped 表示字符串内上一字符为反斜杠，当前引号不应结束字符串。
    escaped = False
    # index 从传入左括号开始逐字符扫描，终点不超过完整文本长度。
    for index in range(open_index, len(text)):
        # character 保存当前字符，避免重复索引并让分支语义清晰。
        character = text[index]
        # 字符串内部只处理转义和闭合引号，不解释括号。
        if quote is not None:
            # 已转义字符仅清除状态，不参与其它判断。
            if escaped:
                escaped = False
                continue
            # 反斜杠使下一个字符转义。
            if character == "\\":
                escaped = True
                continue
            # 与起始引号相同的字符结束字符串。
            if character == quote:
                quote = None
            continue
        # 单引号或双引号开始普通字符串；声明中的注释文本不会破坏括号层级。
        if character in {"'", '"'}:
            quote = character
            continue
        # 左括号进入下一层。
        if character == "(":
            depth += 1
            continue
        # 右括号退出一层；回到零时找到与起点匹配的位置。
        if character == ")":
            depth -= 1
            if depth == 0:
                return index
    # 返回 -1 表示声明被截断；调用方跳过并由编译器/其它测试报告语法错误。
    return -1


def _split_top_level_parameters(parameter_text: str, base_offset: int) -> list[tuple[str, int]]:
    """按顶层逗号拆分参数，并返回每段文本及其在完整源码中的起始偏移。"""

    # parameters 保存参数原文和绝对偏移，供后续精确映射行号。
    parameters: list[tuple[str, int]] = []
    # depth 记录圆括号、方括号和尖括号的近似嵌套总层级，避免泛型/数组内逗号误切。
    depth = 0
    # segment_start 指向当前参数段的首字符。
    segment_start = 0
    # 遍历参数文本，只有 depth 为零的逗号才分隔参数。
    for index, character in enumerate(parameter_text):
        # 三类左括号进入嵌套；尖括号用于 C# 泛型，C 比较表达式不会出现在声明参数类型中。
        if character in "([<":
            depth += 1
        # 三类右括号退出嵌套，max 防止注释或默认值中的孤立字符产生负层级。
        elif character in ")]>":
            depth = max(0, depth - 1)
        # 顶层逗号结束当前参数段。
        elif character == "," and depth == 0:
            parameters.append((parameter_text[segment_start:index], base_offset + segment_start))
            segment_start = index + 1
    # 追加最后一个参数段；空参数列表会在调用方过滤。
    parameters.append((parameter_text[segment_start:], base_offset + segment_start))
    # normalized_parameters 去除每段前导空白，并把偏移推进到参数首个有效字符。
    normalized_parameters: list[tuple[str, int]] = []
    # 遍历原始参数段，空参数与 C 的 void 标记不进入合同审计。
    for raw_parameter, raw_offset in parameters:
        # stripped_parameter 只去除前导空白，保留尾部同行注释供中文检测。
        stripped_parameter = raw_parameter.lstrip()
        # 空段或 void 表示没有真实参数，直接跳过。
        if stripped_parameter.strip() in {"", "void"}:
            continue
        # leading_count 是被删除的换行与缩进字符数，用于精确恢复源码行号。
        leading_count = len(raw_parameter) - len(stripped_parameter)
        # 保存规范化文本和参数首字符绝对偏移。
        normalized_parameters.append((stripped_parameter, raw_offset + leading_count))
    # 返回真正参数，顺序与声明完全一致。
    return normalized_parameters


def _iter_function_declarations(text: str, relative_path: Path) -> list[dict[str, object]]:
    """提取公开/可审计函数声明及参数，返回位置、前缀、参数与就近文档。"""

    # declarations 保存结构化声明；字典避免为审计脚本引入额外数据类样板。
    declarations: list[dict[str, object]] = []
    # lines 提供行级文档和行号查找；keepends=True 保留偏移与原文本一致。
    lines = text.splitlines(keepends=True)
    # line_offsets 保存每行在完整文本中的起始偏移。
    line_offsets: list[int] = []
    # running_offset 累加此前所有行长度。
    running_offset = 0
    # 遍历行并记录偏移，供字符位置转换为一基行号。
    for line in lines:
        line_offsets.append(running_offset)
        running_offset += len(line)
    # 扫描全部左括号；声明前缀过滤会排除普通函数调用。
    for open_match in re.finditer(r"\(", text):
        # open_index 是候选参数列表左括号的绝对字符偏移。
        open_index = open_match.start()
        # line_start 找到当前物理行开头，声明前缀只取本行以避免吸收上一条语句。
        line_start = text.rfind("\n", 0, open_index) + 1
        # prefix 保存函数名之前文本并去除首尾空白。
        prefix = text[line_start:open_index].strip()
        # 控制语句、表达式调用或不匹配声明语法的候选直接跳过。
        if NON_DECLARATION_PREFIX_PATTERN.search(prefix) or not DECLARATION_PREFIX_PATTERN.search(prefix):
            continue
        # close_index 定位同层右括号；截断声明不进入合同审计。
        close_index = _find_matching_parenthesis(text, open_index)
        if close_index < 0:
            continue
        # 参数列表原文不含外层括号。
        parameter_text = text[open_index + 1:close_index]
        # 参数段偏移以左括号后一字符为基准。
        parameters = _split_top_level_parameters(parameter_text, open_index + 1)
        # declaration_line_index 使用换行计数获得零基行号；声明数量小，线性计数开销可忽略。
        declaration_line_index = text.count("\n", 0, line_start)
        # doc_lines 从声明上一行向前收集连续空白/注释行，遇到代码立即停止。
        doc_lines: list[str] = []
        # previous_index 指向声明物理行上一行。
        previous_index = declaration_line_index - 1
        # 最多回溯 40 行，足够覆盖详细 @param 文档且避免吸收上一函数说明。
        while previous_index >= 0 and declaration_line_index - previous_index <= 40:
            # candidate 是当前回溯行的去空白文本。
            candidate = lines[previous_index].strip()
            # 空白、C/C++/C# 注释和 Python 注释均属于就近文档块。
            if not candidate or candidate.startswith(("/*", "*", "*/", "//", "///", "#")):
                doc_lines.insert(0, lines[previous_index].rstrip("\r\n"))
                previous_index -= 1
                continue
            # 首个非注释代码行终止回溯。
            break
        # 从前缀末尾提取函数或构造器名称，失败时使用占位符保证错误仍可定位。
        name_matches = re.findall(r"[A-Za-z_][A-Za-z0-9_]*", prefix)
        # function_name 取最后标识符，对 `public ManifestV1` 和 `int infer` 均正确。
        function_name = name_matches[-1] if name_matches else "<unknown>"
        # 保存声明信息；参数项保持原始片段和绝对偏移。
        declarations.append(
            {
                "name": function_name,
                "prefix": prefix,
                "line": declaration_line_index + 1,
                "line_index": declaration_line_index,
                "parameters": parameters,
                "doc": "\n".join(doc_lines),
                "lines": lines,
                "path": relative_path,
            }
        )
    # 返回按源码顺序提取的声明。
    return declarations


def _parameter_name(parameter_text: str) -> str:
    """从 C/C++/C#/Python 参数片段提取业务参数名，无法识别时返回规范化片段。"""

    # 删除行注释和块注释，防止注释中的中文或标识符被误当参数名。
    without_comments = re.sub(r"//.*?$|/\*.*?\*/|#.*?$", " ", parameter_text, flags=re.MULTILINE | re.DOTALL)
    # 删除默认值；等号右侧可能含其它标识符，不属于参数名。
    without_default = without_comments.split("=", 1)[0].strip()
    # 尝试匹配数组后缀之前的最后标识符。
    match = PARAMETER_NAME_PATTERN.search(without_default)
    # 匹配成功返回精确名称；否则返回压缩片段供错误定位。
    return match.group(1) if match else " ".join(without_default.split())


def audit_enum_member_comments(text: str, relative_path: Path) -> list[str]:
    """要求 C/C++/C# 枚举每个成员具备同一行或紧邻上一行的专属中文语义。"""

    # errors 汇总所有漏注释成员，一次执行给出完整修复清单。
    errors: list[str] = []
    # lines 用于逐行状态机解析枚举体。
    lines = text.splitlines()
    # inside_enum 表示当前位于 enum 左右花括号之间。
    inside_enum = False
    # brace_depth 跟踪枚举体嵌套深度；通常为 1，但赋值初始化可能含花括号。
    brace_depth = 0
    # 逐行解析并保留一基行号。
    for line_index, raw_line in enumerate(lines):
        # stripped_line 仅用于语法判断，中文检测仍使用原行。
        stripped_line = raw_line.strip()
        # 枚举开始必须同时出现 enum 关键字和左花括号；多行 enum 名称由后续左括号暂不支持。
        if not inside_enum and re.search(r"\benum\b[^;{]*\{", stripped_line):
            inside_enum = True
            brace_depth = stripped_line.count("{") - stripped_line.count("}")
            continue
        # 枚举体外不做成员检查。
        if not inside_enum:
            continue
        # 当前行闭合最外层枚举时先更新深度；`}` 后的 typedef 名不是成员。
        next_depth = brace_depth + stripped_line.count("{") - stripped_line.count("}")
        if stripped_line.startswith("}") and next_depth <= 0:
            inside_enum = False
            brace_depth = 0
            continue
        # 空行、注释、属性和预处理行不代表枚举成员。
        if not stripped_line or stripped_line.startswith(("/*", "*", "//", "///", "#", "[")):
            brace_depth = next_depth
            continue
        # 只在枚举直接层解析成员，忽略赋值表达式内部结构。
        if brace_depth == 1:
            member_match = ENUM_MEMBER_PATTERN.fullmatch(stripped_line)
            # 无法匹配的复杂成员由编译器和人工审查处理，不制造错误成员名。
            if member_match:
                # member_name 是错误中展示的枚举常量标识符。
                member_name = member_match.group(1)
                # previous_line 只取紧邻物理上一行，防止一个总述被错误复用于多个成员。
                previous_line = lines[line_index - 1] if line_index > 0 else ""
                # 同行或紧邻上一行任一含中文即可证明该成员有专属语义。
                if not CHINESE_PATTERN.search(raw_line) and not CHINESE_PATTERN.search(previous_line):
                    errors.append(
                        f"{relative_path.as_posix()}:{line_index + 1} 枚举成员 {member_name} 缺少逐项中文语义"
                    )
        # 保存下一行使用的花括号深度。
        brace_depth = next_depth
    # 返回所有成员错误；空列表表示逐项合同完整。
    return errors


def audit_long_parameter_comments(text: str, relative_path: Path) -> list[str]:
    """要求八项及以上声明的每个参数都有中文名称、用途或边界说明。"""

    # C/C++ 实现文件的参数合同由公开头文件统一维护，避免声明和定义重复两套注释后发生漂移。
    if relative_path.suffix.lower() in {".c", ".cc", ".cpp", ".cxx", ".ino"}:
        return []
    # errors 汇总长参数表中未获专属说明的参数。
    errors: list[str] = []
    # 遍历轻量解析器确认的函数、构造器或 record 声明。
    for declaration in _iter_function_declarations(text, relative_path):
        # C# 只审计 public 跨层值对象/接口；private 绘图辅助函数不属于公开 ABI。
        if relative_path.suffix.lower() == ".cs" and not str(declaration["prefix"]).startswith("public "):
            continue
        # parameters 的静态类型由解析器保证为 `(文本, 偏移)` 列表。
        parameters = declaration["parameters"]
        # 少于八项不属于长记录/长 API，本规则不介入。
        if not isinstance(parameters, list) or len(parameters) < 8:
            continue
        # doc 保存声明前连续文档；支持 `@param name 中文` 形式。
        doc = str(declaration["doc"])
        # source_lines 用于检查参数紧邻上一物理行的中文说明。
        source_lines = text.splitlines()
        # 逐项核对，避免只给整个构造器一段模糊总述。
        for parameter_text, parameter_offset in parameters:
            # parameter_name 用于查找 @param 文档并输出可操作错误。
            parameter_name = _parameter_name(parameter_text)
            # parameter_line 是参数首字符所在的一基行号。
            parameter_line = text.count("\n", 0, parameter_offset) + 1
            # previous_line 只接受紧邻上一行，避免上一参数说明被跨行复用。
            previous_line = source_lines[parameter_line - 2] if parameter_line >= 2 else ""
            # inline_or_adjacent 表示参数片段本身或紧邻上一行含中文。
            inline_or_adjacent = bool(
                CHINESE_PATTERN.search(parameter_text) or CHINESE_PATTERN.search(previous_line)
            )
            # documented_by_name 接受声明前文档中的具名中文说明，例如 `@param deviceId 设备标识`。
            documented_by_name = any(
                parameter_name in doc_line and CHINESE_PATTERN.search(doc_line)
                for doc_line in doc.splitlines()
            )
            # 两种形式均缺失时报告精确声明、参数和行号。
            if not inline_or_adjacent and not documented_by_name:
                errors.append(
                    f"{relative_path.as_posix()}:{parameter_line} 长参数声明 {declaration['name']} "
                    f"的参数 {parameter_name} 缺少逐项中文说明"
                )
    # 返回全部长参数合同问题。
    return errors


def audit_public_array_pointer_contracts(text: str, relative_path: Path) -> list[str]:
    """审计公开高风险数组/通用指针 API 的形状、单位、可空性和生命周期合同。"""

    # 只有头文件声明形成跨组件公开 ABI；实现文件内部静态函数不在本规则范围。
    if relative_path.suffix.lower() not in {".h", ".hpp"}:
        return []
    # errors 汇总每个公开函数缺失的合同要素。
    errors: list[str] = []
    # 遍历头文件声明，普通标量 API 不触发高风险合同检查。
    for declaration in _iter_function_declarations(text, relative_path):
        # parameters 转为字符串拼接，便于识别数组和通用 void 指针。
        parameters = declaration["parameters"]
        if not isinstance(parameters, list):
            continue
        # parameter_text 合并所有参数原文；注释不会影响关键符号识别。
        parameter_text = "\n".join(str(item[0]) for item in parameters)
        # has_array 表示 C 数组参数；至少需要形状与单位合同。
        has_array = "[" in parameter_text and "]" in parameter_text
        # has_generic_pointer 只锁定 void 指针或二级指针，避免对每个普通结构体指针制造海量误报。
        has_generic_pointer = bool(re.search(r"\bvoid\s*\*|\*\s*\*", parameter_text))
        # 没有数组和通用指针时交由长参数与一般中文密度规则覆盖。
        if not has_array and not has_generic_pointer:
            continue
        # contract_text 包含声明前文档与参数内联注释，两处均可承载公开 ABI 合同。
        contract_text = f"{declaration['doc']}\n{parameter_text}"
        # 数组必须给出形状，例如 `[62][6]` 或 `[11]`。
        if has_array and "形状" not in contract_text:
            errors.append(
                f"{relative_path.as_posix()}:{declaration['line']} 公开数组 API {declaration['name']} 缺少形状说明"
            )
        # 数组必须声明物理单位；logits 等分数可显式写无量纲。
        if has_array and not any(keyword in contract_text for keyword in ARRAY_UNIT_KEYWORDS):
            errors.append(
                f"{relative_path.as_posix()}:{declaration['line']} 公开数组 API {declaration['name']} 缺少单位/无量纲说明"
            )
        # 通用指针必须明确由谁持有、借用持续到何时。
        if has_generic_pointer and "生命周期" not in contract_text:
            errors.append(
                f"{relative_path.as_posix()}:{declaration['line']} 通用指针 API {declaration['name']} 缺少生命周期说明"
            )
        # 通用指针必须明确是否允许 NULL。
        if has_generic_pointer and not any(keyword in contract_text for keyword in POINTER_NULLABILITY_KEYWORDS):
            errors.append(
                f"{relative_path.as_posix()}:{declaration['line']} 通用指针 API {declaration['name']} 缺少可空性说明"
            )
    # 返回全部公开 ABI 合同问题。
    return errors


def run_git_lines(repository_root: Path, arguments: list[str]) -> list[str]:
    """执行只读 Git 查询，并返回去除空白后的仓库相对路径列表。"""

    # 组合 Git 可执行文件和调用参数；cwd 固定为仓库根，防止相对路径漂移。
    command = ["git", *arguments]
    # 运行 Git 并捕获 UTF-8 文本；check=True 让仓库状态异常立即变成审计失败。
    result = subprocess.run(
        command,
        cwd=repository_root,
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    # 逐行剔除首尾空白和空行，返回可直接构造 Path 的相对名称。
    return [line.strip() for line in result.stdout.splitlines() if line.strip()]


def _filter_auditable_sources(repository_root: Path, raw_paths: list[str]) -> list[Path]:
    """过滤 Git 路径，只保留现存源码与必须触发专用合同的字体生成包成员。"""

    # unique_paths 统一 Windows 与 POSIX 分隔符并去重，避免同一路径被重复审计。
    unique_paths = {Path(path.replace("\\", "/")) for path in raw_paths}
    # source_paths 保存实际存在且属于源码、构建入口或字体生成合同的仓库相对路径。
    source_paths = [
        path
        for path in unique_paths
        if (repository_root / path).is_file()
        and (
            path.suffix.lower() in SOURCE_SUFFIXES
            or path.name in SPECIAL_SOURCE_NAMES
            or path in GENERATED_UI_FONT_AUDIT_PATHS
        )
    ]
    # 按 POSIX 路径排序，保证 Windows、本地 CI 与 GitHub Actions 输出稳定。
    return sorted(source_paths, key=lambda path: path.as_posix().lower())


def collect_changed_sources(repository_root: Path) -> list[Path]:
    """收集相对 HEAD 已修改、已暂存或未跟踪的源码及生成字体审计工件。"""

    # `git diff HEAD` 同时覆盖已暂存和未暂存的已跟踪文件。
    tracked_paths = run_git_lines(
        repository_root,
        ["diff", "--name-only", "--diff-filter=ACMR", "HEAD"],
    )
    # `ls-files --others --exclude-standard` 只返回未跟踪且不受 .gitignore 排除的文件。
    untracked_paths = run_git_lines(
        repository_root,
        ["ls-files", "--others", "--exclude-standard"],
    )
    # 复用统一过滤器；默认差异模式继续覆盖未跟踪源码，不改变既有提交前行为。
    return _filter_auditable_sources(repository_root, tracked_paths + untracked_paths)


def collect_all_tracked_sources(repository_root: Path) -> list[Path]:
    """使用 git ls-files 收集全部已跟踪源码，不把本地未跟踪实验文件混入全仓库审计。"""

    # 不带路径参数的 `git ls-files` 返回索引中的全部跟踪文件，适合开源发布前完整审计。
    tracked_paths = run_git_lines(repository_root, ["ls-files"])
    # 统一过滤源码扩展名和生成包成员；模型头、特征头及字体合同仍由主流程专门验证。
    return _filter_auditable_sources(repository_root, tracked_paths)


def build_argument_parser() -> argparse.ArgumentParser:
    """构造中文命令行帮助；默认审计当前差异，--all 审计全部 Git 跟踪源码。"""

    # parser 说明两种模式的边界，避免教程维护者误以为默认命令已覆盖历史源码。
    parser = argparse.ArgumentParser(
        description=(
            "检查源码是否满足 Agents.md 中文注释合同。"
            "默认只审计相对 HEAD 的已修改、已暂存和未跟踪源码；"
            "开源发布前可使用 --all 审计全部 Git 跟踪源码。"
        ),
        epilog=(
            "示例：python tools/check_agents_compliance.py "
            "或 python tools/check_agents_compliance.py --all"
        ),
        add_help=False,
    )
    # 显式提供中文帮助选项，避免 argparse 默认英文说明混入面向中文教程维护者的 CLI。
    parser.add_argument(
        "-h",
        "--help",
        action="help",
        help="显示此帮助信息并退出。",
    )
    # --all 显式切换为全仓库模式；未提供时保持历史差异模式，兼容现有提交与 CI 流程。
    parser.add_argument(
        "--all",
        dest="audit_all",
        action="store_true",
        help=(
            "使用 git ls-files 审计全部已跟踪源码；"
            "不包含未跟踪文件，且继续执行模型、特征和中文字体生成物专用合同。"
        ),
    )
    # argparse 未提供公开的分组标题本地化参数；此处只改稳定的选项组标题，不改变解析语义。
    parser._optionals.title = "选项"
    # 返回可由 main 和单元测试共同使用的解析器，确保帮助文案与实际参数一致。
    return parser


def sha256_file(path: Path) -> str:
    """流式计算文件 SHA-256，避免把大型模型头或 NPZ 一次载入内存。"""

    # 创建 SHA-256 累加器，输出与 dual_m0_manifest.json 使用相同小写十六进制格式。
    digest = hashlib.sha256()
    # 以二进制只读方式打开文件，保证换行符不会被文本模式改写。
    with path.open("rb") as stream:
        # 每轮读取 1 MiB；空字节串表示到达文件末尾并终止循环。
        while chunk := stream.read(1024 * 1024):
            # 将当前块加入摘要状态，直到覆盖完整文件。
            digest.update(chunk)
    # 返回 64 字符小写十六进制摘要，供清单精确比较。
    return digest.hexdigest()


def _extract_presenter_cjk_symbols(presenter_text: str) -> set[str]:
    """提取 presenter 字符串字面量中的汉字，并加入生成器固定支持的全角标点。"""

    # required_symbols 从固定标点开始，确保清单不能删除生成器声明的基础中文标点。
    required_symbols = set(UI_FONT_REQUIRED_PUNCTUATION)
    # 遍历每个 C 字符串；中文注释不会匹配，因此不会无意义增大字库合同。
    for literal_match in C_STRING_LITERAL_PATTERN.finditer(presenter_text):
        # 遍历字符串中的常用汉字和扩展 A 区字符，与项目中文检测范围保持一致。
        for character_match in CHINESE_PATTERN.finditer(literal_match.group(0)):
            # 把单个 Unicode 字符加入集合；重复文案不会重复计数。
            required_symbols.add(character_match.group(0))
    # 返回用户可见汉字与固定标点集合，调用方据此检查 manifest 全覆盖。
    return required_symbols


def validate_generated_ui_fonts(repository_root: Path) -> list[str]:
    """验证三字号中文字体生成包的授权、规格、字符覆盖与 SHA-256 完整性。"""

    # errors 汇总全部问题，便于一次修复缺字、授权或摘要错误。
    errors: list[str] = []
    # manifest_path 指向字体生成器输出的权威 JSON 清单。
    manifest_path = repository_root / GENERATED_UI_FONT_MANIFEST
    # license_path 指向必须随字体子集分发的 SIL OFL 1.1 完整文本。
    license_path = repository_root / GENERATED_UI_FONT_LICENSE
    # presenter_path 指向所有设备用户可见文案；缺失时无法证明字形覆盖。
    presenter_path = repository_root / UI_PRESENTER_SOURCE
    # 清单不存在时其余规格和摘要均无权威来源，立即返回明确错误。
    if not manifest_path.is_file():
        return [f"中文字体清单缺失：{GENERATED_UI_FONT_MANIFEST.as_posix()}"]
    # presenter 不存在表示字体覆盖输入丢失，禁止接受生成例外。
    if not presenter_path.is_file():
        return [f"中文字体文案源缺失：{UI_PRESENTER_SOURCE.as_posix()}"]
    # 许可证缺失时字体位图不能作为可分发工件进入固件。
    if not license_path.is_file():
        errors.append(f"中文字体许可证缺失：{GENERATED_UI_FONT_LICENSE.as_posix()}")
    else:
        # 读取许可证全文，验证它不是同名空文件或错误授权文本。
        license_text = license_path.read_text(encoding="utf-8-sig")
        # OFL 标题和版本必须同时存在；大小写固定为官方文本格式。
        if "SIL OPEN FONT LICENSE Version 1.1" not in license_text:
            errors.append("OFL-1.1.txt 缺少 SIL Open Font License Version 1.1 正文")
    # JSON 损坏应转换为可读合规错误，不让调用者只看到解析堆栈。
    try:
        # manifest 是字体生成器写出的对象，后续字段按不可信输入逐项验证。
        manifest = json.loads(manifest_path.read_text(encoding="utf-8-sig"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exception:
        # 返回解析错误并保留异常原因，便于重新运行字体生成器修复。
        errors.append(f"中文字体清单无法解析：{exception}")
        return errors
    # 清单自身必须明确 OFL 1.1 和仓库内许可证路径，防止授权信息漂移。
    if manifest.get("license") != "SIL Open Font License 1.1":
        errors.append("中文字体清单 license 不是 SIL Open Font License 1.1")
    # license_file 必须指向本审计器实际验证的文件，不能改到仓库外或其它文本。
    if manifest.get("license_file") != GENERATED_UI_FONT_LICENSE.as_posix():
        errors.append("中文字体清单 license_file 与固定 OFL-1.1.txt 路径不一致")
    # symbols_text 是生成器传给 lv_font_conv 的中文字符集合。
    symbols_text = manifest.get("chinese_symbols")
    # 非字符串字符集无法安全参与 Unicode 覆盖检查，按空集合处理并报告。
    if not isinstance(symbols_text, str):
        errors.append("中文字体清单 chinese_symbols 必须是字符串")
        manifest_symbols: set[str] = set()
    else:
        # 集合化用于比较覆盖；生成器本身会排序并去重。
        manifest_symbols = set(symbols_text)
        # chinese_glyph_count 必须与去重后的实际字符数一致。
        if manifest.get("chinese_glyph_count") != len(manifest_symbols):
            errors.append("中文字体清单 chinese_glyph_count 与 chinese_symbols 不一致")
    # 从真实 presenter 字符串重新计算必须覆盖的 CJK 字符，不能只相信清单声明。
    required_symbols = _extract_presenter_cjk_symbols(
        presenter_path.read_text(encoding="utf-8-sig")
    )
    # missing_symbols 按 Unicode 排序，错误信息可直接用于重跑生成器定位缺字。
    missing_symbols = sorted(required_symbols - manifest_symbols)
    # 任一用户可见字符缺失都会在 LVGL 上显示方框，因此禁止构建通过。
    if missing_symbols:
        errors.append(f"中文字体清单缺少 ui_presenter.c 字符：{''.join(missing_symbols)}")
    # generated_fonts 必须是四个对象组成的列表，不能用字符串或空值绕过迭代检查。
    generated_fonts = manifest.get("generated_fonts")
    # 类型或数量错误时仍继续检查磁盘文件存在性，给出完整缺失列表。
    if not isinstance(generated_fonts, list):
        errors.append("中文字体清单 generated_fonts 必须是列表")
        generated_fonts = []
    # expected_specs 固定四种字号、2 bpp、C 符号和相对路径，禁止部署层级静默变化。
    expected_specs = {
        16: ("ui_font_noto_sans_sc_16", GENERATED_UI_FONT_FILES[0]),
        20: ("ui_font_noto_sans_sc_20", GENERATED_UI_FONT_FILES[1]),
        28: ("ui_font_noto_sans_sc_28", GENERATED_UI_FONT_FILES[2]),
        36: ("ui_font_noto_sans_sc_36", GENERATED_UI_FONT_FILES[3]),
    }
    # records_by_size 将清单记录按字号索引；重复字号不能覆盖后隐藏，因此单独报告。
    records_by_size: dict[int, dict[str, object]] = {}
    # 遍历清单记录并拒绝非对象成员。
    for record in generated_fonts:
        # 每项必须是 JSON 对象，才能安全读取 name、size、bpp 和摘要。
        if not isinstance(record, dict):
            errors.append("中文字体清单 generated_fonts 含非对象成员")
            continue
        # size_value 保存未经转换的字号，布尔值也不接受为整数规格。
        size_value = record.get("size_px")
        # 只允许真实整数；异常类型交由规格集合错误统一报告。
        if not isinstance(size_value, int) or isinstance(size_value, bool):
            errors.append("中文字体清单 size_px 必须是整数")
            continue
        # 同一字号出现两次会使实际链接对象不明确，必须拒绝。
        if size_value in records_by_size:
            errors.append(f"中文字体清单重复字号：{size_value}px")
            continue
        # 保存当前字号记录供固定规格逐项核验。
        records_by_size[size_value] = record
    # 字号集合必须严格为 16、20、28、36，既不能缺失也不能夹带未审计字号。
    if set(records_by_size) != set(expected_specs):
        errors.append(
            "中文字体字号集合必须严格为 16/20/28/36px："
            f"actual={sorted(records_by_size)}"
        )
    # 遍历四个固定规格；即使清单漏项也继续检查对应磁盘文件是否存在。
    for size_px, (expected_name, relative_file) in expected_specs.items():
        # font_path 绑定工作树内字体 C 文件，禁止读取 manifest 提供的任意外部路径。
        font_path = repository_root / relative_file
        # 文件缺失时记录明确路径并跳过文本、长度和摘要检查。
        if not font_path.is_file():
            errors.append(f"中文字体文件缺失：{relative_file.as_posix()}")
            continue
        # 当前字号清单缺失时文件虽存在也不能证明生成参数和摘要，继续下一字号。
        record = records_by_size.get(size_px)
        if record is None:
            continue
        # 名称必须与 renderer 声明的 lv_font_t C 符号一致。
        if record.get("name") != expected_name:
            errors.append(f"{size_px}px 中文字体 name 与固定 C 符号不一致")
        # 位深固定 2 bpp，在可读性与 Flash 体积之间保持已审计取舍。
        if record.get("bpp") != 2:
            errors.append(f"{size_px}px 中文字体 bpp 必须为 2")
        # 文件路径必须与 CMake 编译的仓库相对路径一致，禁止清单指向其它副本。
        if record.get("file") != relative_file.as_posix():
            errors.append(f"{size_px}px 中文字体 file 与固定路径不一致")
        # 使用 UTF-8 读取中文生成头和 lv_font_t 声明；位图数值仍保持原字节用于哈希。
        font_text = font_path.read_text(encoding="utf-8-sig")
        # 生成头必须说明中文字体、禁止手改和授权入口，满足生成代码例外的人审合同。
        for marker in ("自动生成中文界面字体", "禁止手工调整数组", "OFL-1.1.txt"):
            # 任一标记缺失都表示该大数组可能不是项目官方脚本输出。
            if marker not in font_text:
                errors.append(f"{size_px}px 中文字体缺少生成头标记：{marker}")
        # 转换器参数注释必须保留完整字符集合，证明 manifest 的覆盖集合确实进入生成命令。
        if isinstance(symbols_text, str) and symbols_text not in font_text:
            errors.append(f"{size_px}px 中文字体未声明完整 --symbols 字符集合")
        # C 文件必须导出与清单名称一致的 lv_font_t 对象，防止链接阶段找不到字体。
        if f"const lv_font_t {expected_name}" not in font_text:
            errors.append(f"{size_px}px 中文字体缺少 lv_font_t 符号 {expected_name}")
        # 真板已确认压缩字体 bitmap_format=1 出现文字重影；厂家稳定基线使用非压缩格式 0。
        if ".bitmap_format = 0" not in font_text:
            errors.append(f"{size_px}px 中文字体必须使用非压缩 bitmap_format=0")
        # bytes 字段也纳入检查，截断文件会在摘要之外得到更直观错误。
        if record.get("bytes") != font_path.stat().st_size:
            errors.append(f"{size_px}px 中文字体 bytes 与实际文件长度不一致")
        # 计算完整 C 文件 SHA-256，任何手工位图或注释修改都会被发现。
        actual_hash = sha256_file(font_path)
        # 清单摘要必须精确匹配，不接受大小写或部分摘要。
        if record.get("sha256") != actual_hash:
            errors.append(
                f"{size_px}px 中文字体 SHA-256 与清单不一致："
                f"expected={record.get('sha256', '')} actual={actual_hash}"
            )
    # 返回整包全部错误；空列表表示生成例外具备可审计来源和完整性。
    return errors


def validate_generated_header(repository_root: Path) -> list[str]:
    """验证唯一生成代码例外具备声明、结构注释和未篡改哈希。"""

    # 建立错误列表；空列表表示生成头满足例外合同。
    errors: list[str] = []
    # 拼出模型头绝对路径，读取时不依赖调用者当前目录。
    header_path = repository_root / GENERATED_MODEL_HEADER
    # 拼出清单绝对路径，清单缺失时不能允许生成代码例外。
    manifest_path = repository_root / GENERATED_MANIFEST
    # 任一关键工件缺失都立即返回明确路径错误，避免后续出现模糊异常。
    if not header_path.is_file() or not manifest_path.is_file():
        return ["自动生成例外缺少模型头或 dual_m0_manifest.json"]
    # 以 UTF-8 读取生成头；生成器固定输出 UTF-8 中文注释。
    header_text = header_path.read_text(encoding="utf-8")
    # 自动生成声明必须明确禁止手工改权重，防止例外成为普通源码逃逸通道。
    if "自动生成" not in header_text or "禁止手工调整权重" not in header_text:
        errors.append("esp32_dual_m0_model.h 缺少中文自动生成与禁止手改声明")
    # 六组顺序说明必须存在，保证数组索引合同可由人审查。
    if "六组输入维度" not in header_text or "顺序不得与 Python" not in header_text:
        errors.append("esp32_dual_m0_model.h 缺少六分支维度或 Python 顺序说明")
    # 拆分全部行，逐一核对每个 float 数组前最近的非空行是否说明原形状与字节数。
    header_lines = header_text.splitlines()
    # 遍历生成头中的每一行，定位模型参数和 scaler 数组声明。
    for line_index, line in enumerate(header_lines):
        # 非 float 常量数组不进入形状检查，继续下一行。
        if not FLOAT_ARRAY_PATTERN.search(line):
            continue
        # 从声明上一行向前跳过空行，找到该数组的最近说明。
        previous_index = line_index - 1
        # 空白行不携带语义，持续向前查找直到文件开头。
        while previous_index >= 0 and not header_lines[previous_index].strip():
            previous_index -= 1
        # 最近说明必须含中文，并同时给出原形状和字节数。
        previous_line = header_lines[previous_index] if previous_index >= 0 else ""
        # 任一要素缺失时记录数组声明行号，便于修复生成器模板。
        if not CHINESE_PATTERN.search(previous_line) or "形状" not in previous_line or "字节" not in previous_line:
            errors.append(f"生成数组第 {line_index + 1} 行前缺少中文形状/字节说明")
    # 读取清单 JSON；UTF-8 与导出器输出合同一致。
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # 从 generated_files 读取权威摘要；字段缺失时使用空字符串并让比较失败。
    expected_hash = str(manifest.get("generated_files", {}).get(GENERATED_MODEL_HEADER.name, ""))
    # 计算磁盘模型头实际摘要，确保手工修改会被审计拦截。
    actual_hash = sha256_file(header_path)
    # 摘要必须逐字符一致；错误同时打印期望和实际值，方便重新生成定位。
    if expected_hash != actual_hash:
        errors.append(f"模型头 SHA-256 与清单不一致：expected={expected_hash} actual={actual_hash}")
    # 返回所有生成例外错误，由主流程统一打印和设置退出码。
    return errors


def validate_generated_feature_header(repository_root: Path) -> list[str]:
    """验证特征算法头由官方模板生成、包含中文合同且摘要与清单一致。"""

    # errors 汇总生成声明、关键算法说明和清单摘要问题。
    errors: list[str] = []
    # feature_path 指向部署端 297 维特征、清洗和因果状态实现。
    feature_path = repository_root / GENERATED_FEATURE_HEADER
    # manifest_path 指向同一次双 M0 导出生成的权威摘要清单。
    manifest_path = repository_root / GENERATED_MANIFEST
    # 缺少任一文件都无法证明特征头来自官方生成流程。
    if not feature_path.is_file() or not manifest_path.is_file():
        return ["自动生成特征头或 dual_m0_manifest.json 缺失"]
    # 使用 UTF-8 读取中文说明和 C99 算法文本。
    feature_text = feature_path.read_text(encoding="utf-8")
    # 文件头必须明确说明由 Python 导出器生成以及训练/部署一致性要求。
    if "Python 导出器生成" not in feature_text or "必须与训练端保持一致" not in feature_text:
        errors.append("esp32_bp_features.h 缺少中文自动生成与一致性声明")
    # 三个关键主函数必须存在，避免哈希清单意外指向截断或错误模板。
    for function_name in (
        "preprocess_imu_window",
        "extract_features_from_window",
        "bp_bout_accumulator_update",
    ):
        # 任一入口缺失都会让固件无法复现最终 Python 链路。
        if function_name not in feature_text:
            errors.append(f"esp32_bp_features.h 缺少运行时函数 {function_name}")
    # 读取清单并取得特征头期望摘要。
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    # 字段缺失时按空字符串处理，让比较产生明确失败。
    expected_hash = str(manifest.get("generated_files", {}).get(GENERATED_FEATURE_HEADER.name, ""))
    # 实际摘要覆盖磁盘完整 UTF-8 字节，包括所有中文算法注释。
    actual_hash = sha256_file(feature_path)
    # 摘要不一致表示生成后被手改、导出中断或清单来自另一版本。
    if expected_hash != actual_hash:
        errors.append(f"特征头 SHA-256 与清单不一致：expected={expected_hash} actual={actual_hash}")
    # 返回全部合同问题；空列表表示官方生成证据完整。
    return errors


def is_semantic_source_line(stripped_line: str, inside_numeric_array: bool) -> bool:
    """判断当前行是否携带需要就近中文说明的可执行、声明或配置语义。"""

    # 空行和纯格式行只改善排版或闭合结构，不累计连续语义行数。
    if not stripped_line or FORMAT_ONLY_PATTERN.fullmatch(stripped_line):
        return False
    # Python、PowerShell、CMake 和预处理注释行本身不属于待解释语句。
    if stripped_line.startswith(("#", "//", "/*", "*", "*/")):
        return False
    # 三引号、C 原始字符串边界和单独 else/catch 等结构标记不携带独立运算。
    if stripped_line in {"'''", '"""', "r\"\"\"", "else {", "#else", "#endif"}:
        return False
    # 已识别的数值数组区内，纯数字、逗号和花括号由数组前形状说明覆盖。
    if inside_numeric_array and NUMERIC_INITIALIZER_LINE_PATTERN.fullmatch(stripped_line):
        return False
    # XAML 结束标签和只含 XML 结构字符的行只负责布局闭合。
    if stripped_line.startswith("</") and stripped_line.endswith(">"):
        return False
    # 其它非空源码行视为语义行；包括变量、分支、循环、函数调用和配置属性。
    return True


def audit_uncommented_semantic_blocks(text: str, relative_path: Path) -> list[str]:
    """报告全文件最长未获中文说明覆盖的语义代码块，并忽略数值数组与纯格式行。"""

    # current_count 统计上一个中文说明之后连续出现的语义行数。
    current_count = 0
    # current_start 保存当前连续块首个语义行的一基行号。
    current_start = 0
    # longest_count 保存全文件最长块的语义行数，用于只报告最严重缺口。
    longest_count = 0
    # longest_start 保存最长块起始行。
    longest_start = 0
    # longest_end 保存最长块结束行。
    longest_end = 0
    # inside_numeric_array 跟踪 C 静态数值初始化区，避免数千个权重元素形成误报。
    inside_numeric_array = False
    # 逐行扫描保留真实行号，便于开发者直接跳转修复。
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        # stripped_line 去除首尾空白，只用于语法启发式，不改变原文件。
        stripped_line = raw_line.strip()
        # 含中文的代码或注释行会重新建立解释上下文，连续计数归零。
        if CHINESE_PATTERN.search(raw_line):
            current_count = 0
            current_start = 0
            # 中文数组声明也可能打开数值区，因此仍更新数组状态。
            if NUMERIC_ARRAY_START_PATTERN.search(stripped_line):
                inside_numeric_array = True
            continue
        # 普通数值数组声明开启数据区；声明本身仍按语义行处理，要求前方已有中文说明。
        if NUMERIC_ARRAY_START_PATTERN.search(stripped_line):
            inside_numeric_array = True
        # 判断当前行是否属于需解释的语义代码。
        if is_semantic_source_line(stripped_line, inside_numeric_array):
            # 新块首行记录实际行号。
            if current_count == 0:
                current_start = line_number
            # 累加当前语义行。
            current_count += 1
            # 当前块超过历史最长时同步保存长度和边界。
            if current_count > longest_count:
                longest_count = current_count
                longest_start = current_start
                longest_end = line_number
        # 数值数组闭合行结束忽略区；`};` 本身是纯格式，不影响语义计数。
        if inside_numeric_array and stripped_line.startswith("};"):
            inside_numeric_array = False
    # 最长块未越过门槛时返回空列表，表示不存在整段漏注释。
    if longest_count <= MAX_UNCOMMENTED_SEMANTIC_LINES:
        return []
    # 只报告全文件最长块，避免大量次要块淹没最需要修复的位置。
    return [
        f"{relative_path.as_posix()} 最长连续语义块缺少中文说明："
        f"lines={longest_start}-{longest_end} semantic_lines={longest_count} "
        f"limit={MAX_UNCOMMENTED_SEMANTIC_LINES}"
    ]


def audit_source_file(repository_root: Path, relative_path: Path) -> list[str]:
    """按中文行比例和连续语义块长度审计一个普通新增或修改源码文件。"""

    # 将相对路径绑定到仓库根，防止脚本从其它目录启动时误读同名文件。
    absolute_path = repository_root / relative_path
    # 使用 utf-8-sig 同时兼容普通 UTF-8 和 Windows PowerShell 5.1 所需 BOM。
    text = absolute_path.read_text(encoding="utf-8-sig")
    # 非空行代表源码或说明内容；纯格式空行不应稀释注释密度。
    nonblank_lines = [line for line in text.splitlines() if line.strip()]
    # 含至少一个中文字符的行视为中文说明行；支持行尾注释和独立注释行。
    chinese_lines = [line for line in nonblank_lines if CHINESE_PATTERN.search(line)]
    # errors 同时汇总文件级密度和语义块问题，避免一个错误掩盖另一个。
    errors: list[str] = []
    # 空文件没有可审计逻辑，直接返回明确错误。
    if not nonblank_lines:
        return [f"{relative_path.as_posix()} 是空源码文件"]
    # 任一源码完全没有中文时，无论文件长短都违反项目合同。
    if not chinese_lines:
        return [f"{relative_path.as_posix()} 没有中文说明"]
    # 大文件继续执行最低密度启发式检查，小文件由中文存在性和语义块门槛覆盖。
    if len(nonblank_lines) > LARGE_FILE_NONBLANK_THRESHOLD:
        # 计算中文行占非空行比例；该指标只做机器预筛，最终仍需人工审查语义准确性。
        ratio = len(chinese_lines) / len(nonblank_lines)
        # 低于 20% 时报告行数和实际比例，便于快速定位漏注释文件。
        if ratio < MIN_CHINESE_LINE_RATIO:
            errors.append(
                f"{relative_path.as_posix()} 中文行密度过低："
                f"{len(chinese_lines)}/{len(nonblank_lines)}={ratio:.1%}"
            )
    # C 系语言语句边界明确，执行连续语义块检查以发现总体密度合格但某个函数整段未说明的情况。
    if relative_path.suffix.lower() in SEMANTIC_BLOCK_SUFFIXES:
        # Python 多行表达式和 XAML 属性没有稳定分号边界，避免用 C 启发式制造误报。
        errors.extend(audit_uncommented_semantic_blocks(text, relative_path))
        # 枚举逐项审计防止只写总述、却无法判断每个状态值业务语义。
        # 自动生成特征头由生成器哈希和专用结构合同保护，禁止手工补注释破坏摘要。
        if relative_path.as_posix() != GENERATED_FEATURE_HEADER.as_posix():
            # 手写枚举必须逐项说明，生成表则由模板和清单控制。
            errors.extend(audit_enum_member_comments(text, relative_path))
            # 长构造器和长公开函数逐参数审计，避免参数顺序、单位和用途只能靠猜测。
            errors.extend(audit_long_parameter_comments(text, relative_path))
            # 头文件中的数组与通用指针还需形状、单位、可空性和生命周期合同。
            errors.extend(audit_public_array_pointer_contracts(text, relative_path))
    # 返回当前文件全部机器可识别问题。
    return errors


def main(argv: list[str] | None = None) -> int:
    """执行默认差异审计或显式全仓库审计；成功返回 0，失败返回 1。"""

    # 脚本位于仓库根 tools/，父目录即权威项目根。
    repository_root = Path(__file__).resolve().parent.parent
    # 解析命令行；argv 为 None 时读取真实进程参数，测试可传入独立列表避免污染。
    arguments = build_argument_parser().parse_args(argv)
    # --all 使用全部 Git 跟踪源码；默认分支保持历史差异范围和未跟踪源码覆盖。
    source_paths = (
        collect_all_tracked_sources(repository_root)
        if arguments.audit_all
        else collect_changed_sources(repository_root)
    )
    # 没有源码变更时输出确定性通过标记，文档单独变更不应误报。
    if not source_paths:
        print("AGENTS_COMMENT_AUDIT_OK files=0 generated_exceptions=0")
        return 0
    # 累积全部文件错误，一次运行展示完整修复清单，避免逐文件反复执行。
    errors: list[str] = []
    # 记录实际使用的生成例外数量，成功标记可被交付审计解析。
    generated_exception_count = 0
    # font_bundle_validated 防止五个字体包成员变化时重复输出同一整包错误。
    font_bundle_validated = False
    # 遍历按路径排序后的变更源码，保证报告顺序稳定。
    for relative_path in source_paths:
        # 唯一自动生成权重头使用严格清单校验，不使用普通注释比例。
        if relative_path.as_posix() == GENERATED_MODEL_HEADER.as_posix():
            generated_exception_count += 1
            errors.extend(validate_generated_header(repository_root))
            continue
        # 字体位图、清单和许可证任一变化都执行一次整包验证，并跳过普通逐行注释审计。
        if relative_path in GENERATED_UI_FONT_AUDIT_PATHS:
            # 首个字体包成员负责校验三字号、哈希、授权和 presenter 字符覆盖。
            if not font_bundle_validated:
                # 字体包作为一个生成例外计数，避免三个字号被误读成三个独立人工豁免。
                generated_exception_count += 1
                # 追加整包错误；空列表表示所有生成证据相互一致。
                errors.extend(validate_generated_ui_fonts(repository_root))
                # 标记本轮已校验，后续 manifest、license 或其它字号只跳过普通审计。
                font_bundle_validated = True
            # 自动生成位图和配套 JSON/TXT 不进入普通源码中文行密度统计。
            continue
        # 特征头既需普通逐段中文审计，也需清单摘要证明来自模板而非手工修补。
        if relative_path.as_posix() == GENERATED_FEATURE_HEADER.as_posix():
            # 先验证生成声明、关键入口和 SHA-256。
            errors.extend(validate_generated_feature_header(repository_root))
        # 普通自研源码执行中文注释存在性和最低密度检查。
        errors.extend(audit_source_file(repository_root, relative_path))
    # 有错误时逐条打印 ERROR，便于 CI 和人工审计直接检索。
    if errors:
        # 按发现顺序输出每个问题，不吞掉多个文件的并行缺口。
        for error in errors:
            print(f"ERROR {error}")
        # 输出失败汇总，包含审计文件数与生成例外数量。
        print(
            f"AGENTS_COMMENT_AUDIT_FAILED files={len(source_paths)} "
            f"generated_exceptions={generated_exception_count} errors={len(errors)}"
        )
        return 1
    # 成功时输出单行机器标记，供最终交付脚本引用。
    print(
        f"AGENTS_COMMENT_AUDIT_OK files={len(source_paths)} "
        f"generated_exceptions={generated_exception_count}"
    )
    # 返回 0 表示所有变更源码通过机器预筛和生成例外完整性检查。
    return 0


# 仅在直接执行脚本时运行主函数；作为模块导入时不触发 Git 审计。
if __name__ == "__main__":
    # 将 main 返回码交给操作系统，供 PowerShell、CI 和 Codex 判定成败。
    sys.exit(main())
