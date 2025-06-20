from includes import *
from common import *
import csv
from RLTest import Env

def testWITHSUFFIXTRIEParamText(env):
    conn = getConnectionByEnv(env)
    env.expect('ft.create', 'idx', 'schema', 't', 'TEXT', 'SORTABLE', 'WITHSUFFIXTRIE').error()
    env.expect('ft.create', 'idx', 'schema', 't', 'TEXT', 'SORTABLE', 'NOSTEM').error() # sortable must be last

    # without sortable
    env.expect('ft.create', 'idx', 'schema', 't', 'TEXT', 'WITHSUFFIXTRIE').ok()
    res_info = [['identifier', 't', 'attribute', 't', 'type', 'TEXT', 'WEIGHT', '1', 'WITHSUFFIXTRIE']]
    assertInfoField(env, 'idx', 'attributes', res_info)

    # with sortable
    env.expect('ft.create', 'idx_sortable', 'schema', 't', 'TEXT', 'WITHSUFFIXTRIE', 'SORTABLE').ok()
    res_info = [['identifier', 't', 'attribute', 't', 'type', 'TEXT', 'WEIGHT', '1', 'SORTABLE', 'WITHSUFFIXTRIE']]
    assertInfoField(env, 'idx_sortable', 'attributes', res_info)

    # nostem 1st
    env.expect('ft.create', 'idx_nostem1', 'schema', 't', 'TEXT', 'WITHSUFFIXTRIE', 'NOSTEM').ok()
    res_info = [['identifier', 't', 'attribute', 't', 'type', 'TEXT', 'WEIGHT', '1', 'NOSTEM', 'WITHSUFFIXTRIE']]
    assertInfoField(env, 'idx_nostem1', 'attributes', res_info)

    # nostem 2nd
    env.expect('ft.create', 'idx_nostem2', 'schema', 't', 'TEXT', 'NOSTEM', 'WITHSUFFIXTRIE').ok()
    assertInfoField(env, 'idx_nostem2', 'attributes', res_info)

def testWITHSUFFIXTRIEParamTag(env):
    conn = getConnectionByEnv(env)
    env.expect('ft.create', 'idx', 'schema', 't', 'TAG', 'SORTABLE', 'WITHSUFFIXTRIE').error()
    env.expect('ft.create', 'idx', 'schema', 't', 'TAG', 'SORTABLE', 'CASESENSITIVE').error() # sortable must be last

    # without sortable
    env.expect('ft.create', 'idx', 'schema', 't', 'TAG', 'WITHSUFFIXTRIE').ok()
    res_info = [['identifier', 't', 'attribute', 't', 'type', 'TAG', 'SEPARATOR', ',', 'WITHSUFFIXTRIE']]
    assertInfoField(env, 'idx', 'attributes', res_info)

    # with sortable
    env.expect('ft.create', 'idx_sortable', 'schema', 't', 'TAG', 'WITHSUFFIXTRIE', 'SORTABLE').ok()
    res_info = [['identifier', 't', 'attribute', 't', 'type', 'TAG', 'SEPARATOR', ',', 'SORTABLE', 'WITHSUFFIXTRIE']]
    assertInfoField(env, 'idx_sortable', 'attributes', res_info)

    # with casesensitive - automatically set to UNF as well
    env.expect('ft.create', 'idx_casesensitive', 'schema', 't', 'TAG', 'WITHSUFFIXTRIE', 'CASESENSITIVE', 'SORTABLE').ok()
    res_info = [['identifier', 't', 'attribute', 't', 'type', 'TAG', 'SEPARATOR', ',', 'CASESENSITIVE', 'SORTABLE', 'UNF', 'WITHSUFFIXTRIE']]
    assertInfoField(env, 'idx_casesensitive', 'attributes', res_info)

def testBasicContains(env):
    conn = getConnectionByEnv(env)

    env.expect('ft.create', 'idx', 'schema', 'title', 'text', 'body', 'text').ok()
    conn.execute_command('HSET', 'doc1', 'title', 'hello world', 'body', 'this is a test')

    # prefix
    res = env.cmd('ft.search', 'idx', 'worl*')
    env.assertEqual(res[0:2], [1, 'doc1'])
    env.assertEqual(set(res[2]), set(['title', 'hello world', 'body', 'this is a test']))

    # suffix
    res = env.cmd('ft.search', 'idx', '*orld')
    env.assertEqual(res[0:2], [1, 'doc1'])
    env.assertEqual(set(res[2]), set(['title', 'hello world', 'body', 'this is a test']))
    env.expect('ft.search', 'idx', '*orl').equal([0])

    # contains
    res = env.cmd('ft.search', 'idx', '*orl*')
    env.assertEqual(res[0:2], [1, 'doc1'])
    env.assertEqual(set(res[2]), set(['title', 'hello world', 'body', 'this is a test']))

@skip(cluster=True)
def testSanity(env: Env):
    env.expect(config_cmd(), 'set', 'MINPREFIX', 1).ok()
    env.expect(config_cmd(), 'set', 'MAXEXPANSIONS', 10000000).ok()
    item_qty = 1000

    index_list = ['idx_bf', 'idx_suffix']
    env.cmd('ft.create', 'idx_bf', 'SCHEMA', 't', 'TEXT')
    env.cmd('ft.create', 'idx_suffix', 'SCHEMA', 't', 'TEXT', 'WITHSUFFIXTRIE')

    conn = getConnectionByEnv(env)

    pl = conn.pipeline()
    for i in range(item_qty):
        pl.execute_command('HSET', 'doc%d' % i, 't', 'foo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty), 't', 'fooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 2), 't', 'foooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 3), 't', 'foofo%d' % i)
        pl.execute()

    for i in range(2):
        #prefix
        env.expect('ft.search', index_list[i], 'f*', 'LIMIT', 0, 0).equal([4000])
        env.expect('ft.search', index_list[i], 'foo*', 'LIMIT', 0, 0).equal([4000])
        env.expect('ft.search', index_list[i], 'foo1*', 'LIMIT', 0, 0).equal([111])
        env.expect('ft.search', index_list[i], '*ooo1*', 'LIMIT', 0, 0).equal([222])

        # contains
        env.expect('ft.search', index_list[i], '*oo*', 'LIMIT', 0, 0).equal([4000])
        # 55x & x55 - 555
        env.expect('ft.search', index_list[i], '*55*', 'LIMIT', 0, 0).equal([76])
        # 555
        env.expect('ft.search', index_list[i], '*555*', 'LIMIT', 0, 0).equal([4])
        env.expect('ft.search', index_list[i], '*o55*', 'LIMIT', 0, 0).equal([44])
        env.expect('ft.search', index_list[i], '*oo55*', 'LIMIT', 0, 0).equal([33])
        env.expect('ft.search', index_list[i], '*oo555*', 'LIMIT', 0, 0).equal([3])

        # 23x & x23
        env.expect('ft.search', index_list[i], '*23*', 'LIMIT', 0, 0).equal([80])
        # 234
        env.expect('ft.search', index_list[i], '*234*', 'LIMIT', 0, 0).equal([4])
        env.expect('ft.search', index_list[i], '*o23*', 'LIMIT', 0, 0).equal([44])
        env.expect('ft.search', index_list[i], '*oo23*', 'LIMIT', 0, 0).equal([33])
        env.expect('ft.search', index_list[i], '*oo234*', 'LIMIT', 0, 0).equal([3])

        # suffix
        env.expect('ft.search', index_list[i], '*oo234', 'LIMIT', 0, 0).equal([3])
        env.expect('ft.search', index_list[i], '*234', 'LIMIT', 0, 0).equal([4])
        env.expect('ft.search', index_list[i], '*13', 'LIMIT', 0, 0).equal([40])

    # test timeout
    for i in range(item_qty, item_qty * 5):
        pl.execute_command('HSET', 'doc%d' % i, 't', 'foo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty), 't', 'fooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 2), 't', 'foooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 3), 't', 'foofo%d' % i)
        pl.execute()

    env.expect(config_cmd(), 'set', 'TIMEOUT', 1).ok()
    env.expect(config_cmd(), 'set', 'ON_TIMEOUT', 'FAIL').ok()
    env.expect('ft.search', index_list[0], 'foo*', 'LIMIT', 0, 0).error() \
      .contains('Timeout limit was reached')
    env.expect('ft.search', index_list[1], 'foo*', 'LIMIT', 0, 0).error() \
      .contains('Timeout limit was reached')

@skip(cluster=True)
def testSanityTags(env):
    env.expect(config_cmd(), 'set', 'MINPREFIX', 1).ok()
    env.expect(config_cmd(), 'set', 'MAXEXPANSIONS', 10000000).ok()
    item_qty = 1000

    index_list = ['idx_bf', 'idx_suffix']
    env.cmd('ft.create', index_list[0], 'SCHEMA', 't', 'TAG')
    env.cmd('ft.create', index_list[1], 'SCHEMA', 't', 'TAG', 'WITHSUFFIXTRIE')

    conn = getConnectionByEnv(env)

    pl = conn.pipeline()
    for i in range(item_qty):
        pl.execute_command('HSET', 'doc%d' % i, 't', 'foo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty), 't', 'fooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 2), 't', 'foooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 3), 't', 'foofo%d' % i)
        pl.execute()

    for i in range(len(index_list)):
        #prefix
        env.expect('ft.search', index_list[i], '@t:{f*}', 'LIMIT', 0, 0).equal([4000])
        env.expect('ft.search', index_list[i], '@t:{foo*}', 'LIMIT', 0, 0).equal([4000])
        env.expect('ft.search', index_list[i], '@t:{foo1*}', 'LIMIT', 0, 0).equal([111])
        env.expect('ft.search', index_list[i], '@t:{*ooo1*}', 'LIMIT', 0, 0).equal([222])

        # contains
        env.expect('ft.search', index_list[i], '@t:{*oo*}', 'LIMIT', 0, 0).equal([4000])
        # 55x & x55 - 555
        env.expect('ft.search', index_list[i], '@t:{*55*}', 'LIMIT', 0, 0).equal([76])
        # 555
        env.expect('ft.search', index_list[i], '@t:{*555*}', 'LIMIT', 0, 0).equal([4])
        env.expect('ft.search', index_list[i], '@t:{*o55*}', 'LIMIT', 0, 0).equal([44])
        env.expect('ft.search', index_list[i], '@t:{*oo55*}', 'LIMIT', 0, 0).equal([33])
        env.expect('ft.search', index_list[i], '@t:{*oo555*}', 'LIMIT', 0, 0).equal([3])

        # 23x & x23
        env.expect('ft.search', index_list[i], '@t:{*23*}', 'LIMIT', 0, 0).equal([80])
        # 234
        env.expect('ft.search', index_list[i], '@t:{*234*}', 'LIMIT', 0, 0).equal([4])
        env.expect('ft.search', index_list[i], '@t:{*o23*}', 'LIMIT', 0, 0).equal([44])
        env.expect('ft.search', index_list[i], '@t:{*oo23*}', 'LIMIT', 0, 0).equal([33])
        env.expect('ft.search', index_list[i], '@t:{*oo234*}', 'LIMIT', 0, 0).equal([3])

        # suffix
        env.expect('ft.search', index_list[i], '@t:{*oo234}', 'LIMIT', 0, 0).equal([3])
        env.expect('ft.search', index_list[i], '@t:{*234}', 'LIMIT', 0, 0).equal([4])
        env.expect('ft.search', index_list[i], '@t:{*13}', 'LIMIT', 0, 0).equal([40])

    # test timeout
    for i in range(item_qty, item_qty * 5):
        pl.execute_command('HSET', 'doc%d' % i, 't', 'foo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty), 't', 'fooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 2), 't', 'foooo%d' % i)
        pl.execute_command('HSET', 'doc%d' % (i + item_qty * 3), 't', 'foofo%d' % i)
        pl.execute()

    env.expect(config_cmd(), 'set', 'TIMEOUT', 1).ok()
    env.expect(config_cmd(), 'set', 'ON_TIMEOUT', 'FAIL').ok()
    env.expect('ft.search', index_list[0], '@t:{foo*}', 'LIMIT', 0, 0).error() \
      .contains('Timeout limit was reached')
    env.expect('ft.search', index_list[1], '@t:{foo*}', 'LIMIT', 0, 0).error() \
      .contains('Timeout limit was reached')

def testEscape(env):
  # this test check that `\*` is escaped correctly on contains queries
  conn = getConnectionByEnv(env)
  env.cmd('ft.create', 'idx', 'SCHEMA', 't', 'TEXT', 'SORTABLE')
  conn.execute_command('HSET', 'doc1', 't', '1foo1')
  conn.execute_command('HSET', 'doc2', 't', r'\*foo2')
  conn.execute_command('HSET', 'doc3', 't', r'3\*foo3')
  conn.execute_command('HSET', 'doc4', 't', r'4foo\*')
  conn.execute_command('HSET', 'doc5', 't', r'5foo\*5')
  all_docs = [5, 'doc1', ['t', '1foo1'], 'doc2', ['t', r'\*foo2'], 'doc3', ['t', r'3\*foo3'],
                 'doc4', ['t', r'4foo\*'], 'doc5', ['t', r'5foo\*5']]
  # contains
  res = env.cmd('ft.search', 'idx', '*foo*')
  env.assertEqual(toSortedFlatList(res), toSortedFlatList(all_docs))

  # prefix only
  env.expect('ft.search', 'idx', r'\*foo*').equal([1, 'doc2', ['t', r'\*foo2']])
  # suffix only
  env.expect('ft.search', 'idx', r'*foo\*').equal([1, 'doc4', ['t', r'4foo\*']])
  # none
  env.expect('ft.search', 'idx', r'\*foo\*').equal([0])

  env.expect('ft.search', 'idx', r'*\*foo\**').equal([0])


def test_misc1(env):
  # this test check that `\*` is escaped correctly on contains queries
  conn = getConnectionByEnv(env)
  env.cmd('ft.create', 'idx', 'SCHEMA', 't', 'TEXT', 'WITHSUFFIXTRIE', 'SORTABLE')

  conn.execute_command('HSET', 'doc1', 't', 'world')
  conn.execute_command('HSET', 'doc2', 't', 'keyword')
  conn.execute_command('HSET', 'doc3', 't', 'doctorless')
  conn.execute_command('HSET', 'doc4', 't', 'anteriorly')
  conn.execute_command('HSET', 'doc5', 't', 'colorlessness')
  conn.execute_command('HSET', 'doc6', 't', 'floorless')

  # prefix
  res = env.cmd('ft.search', 'idx', 'worl*')
  env.assertEqual(res[0:2], [1, 'doc1'])
  env.assertEqual(set(res[2]), set(['world', 't']))

  # contains
  res = env.cmd('ft.search', 'idx', '*orld*')
  env.assertEqual(res, [1, 'doc1', ['t', 'world']])

  res = env.cmd('ft.search', 'idx', '*orl*')
  actual_res = [5, 'doc1', ['t', 'world'], 'doc3', ['t', 'doctorless'],
                'doc4', ['t', 'anteriorly'], 'doc5', ['t', 'colorlessness'], 'doc6', ['t', 'floorless']]
  env.assertEqual(toSortedFlatList(res), toSortedFlatList(actual_res))

  res = env.cmd('ft.search', 'idx', '*or*')
  actual_res = [6, 'doc1', ['t', 'world'], 'doc2', ['t', 'keyword'], 'doc3', ['t', 'doctorless'],
                'doc4', ['t', 'anteriorly'], 'doc5', ['t', 'colorlessness'], 'doc6', ['t', 'floorless']]
  env.assertEqual(toSortedFlatList(res), toSortedFlatList(actual_res))

  # suffix
  res = env.cmd('ft.search', 'idx', '*orld')
  env.assertEqual(res, [1, 'doc1', ['t', 'world']])

  res = env.cmd('ft.search', 'idx', '*ess')
  actual_res = [3, 'doc3', ['t', 'doctorless'], 'doc5', ['t', 'colorlessness'], 'doc6', ['t', 'floorless']]
  env.assertEqual(toSortedFlatList(res), toSortedFlatList(actual_res))

  res = env.cmd('ft.search', 'idx', '*less')
  actual_res = [2, 'doc3', ['t', 'doctorless'], 'doc6', ['t', 'floorless']]
  env.assertEqual(toSortedFlatList(res), toSortedFlatList(actual_res))

@skip(cluster=True)
def testContainsGC(env):
  env.expect(config_cmd() + ' set FORK_GC_CLEAN_THRESHOLD 0').ok()

  conn = getConnectionByEnv(env)
  conn.execute_command('FT.CREATE', 'idx', 'SCHEMA', 't', 'TEXT', 'WITHSUFFIXTRIE', 'SORTABLE')

  conn.execute_command('HSET', 'doc1', 't', 'hello')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx').equal(['ello', 'hello', 'llo', 'lo'])
  conn.execute_command('HSET', 'doc1', 't', 'world')

  forceInvokeGC(env, 'idx')

  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx').equal(['ld', 'orld', 'rld', 'world'])

  conn.execute_command('HSET', 'doc2', 't', 'bold')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx').equal(['bold', 'ld', 'old', 'orld', 'rld', 'world'])
  conn.execute_command('DEL', 'doc2')

  forceInvokeGC(env, 'idx')

  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx').equal(['ld', 'orld', 'rld', 'world'])

@skip(cluster=True)
def testContainsGCTag(env):
  env.expect(config_cmd() + ' set FORK_GC_CLEAN_THRESHOLD 0').ok()

  conn = getConnectionByEnv(env)
  conn.execute_command('FT.CREATE', 'idx', 'SCHEMA', 't', 'TAG', 'WITHSUFFIXTRIE', 'SORTABLE')

  conn.execute_command('HSET', 'doc1', 't', 'hello')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 't').equal(['ello', 'hello', 'llo', 'lo'])
  conn.execute_command('HSET', 'doc1', 't', 'world')

  forceInvokeGC(env, 'idx')

  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 't').equal(['ld', 'orld', 'rld', 'world'])

  conn.execute_command('HSET', 'doc2', 't', 'bold')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 't').equal(['bold', 'ld', 'old', 'orld', 'rld', 'world'])
  conn.execute_command('DEL', 'doc2')

  forceInvokeGC(env, 'idx')

  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 't').equal(['ld', 'orld', 'rld', 'world'])

@skip(cluster=True)
def testContainsDebugCommand(env):
  conn = getConnectionByEnv(env)
  conn.execute_command('FT.CREATE', 'idx', 'SCHEMA', 'text', 'TEXT', 'WITHSUFFIXTRIE')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 'field').error()  \
    .contains('Could not find given field in index spec')

  conn.execute_command('FT.CREATE', 'idx_no', 'SCHEMA', 'text', 'TEXT')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx_no').error()  \
    .contains('Index does not have suffix trie')

  conn.execute_command('FT.CREATE', 'idx_tag', 'SCHEMA', 'tag', 'TAG', 'WITHSUFFIXTRIE')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 'tag_no').error() \
    .contains('Could not find given field in index spec')
  env.expect(debug_cmd(), 'DUMP_SUFFIX_TRIE', 'idx', 'tag_no', 'tag_yes').error(). \
    contains('wrong number of arguments')

@skip(cluster=True)
def testContainsMixedWithSuffix(env):

  conn = getConnectionByEnv(env)
  conn.execute_command('FT.CREATE', 'idx', 'SCHEMA', 't1', 'TEXT', 'WITHSUFFIXTRIE', 't2', 'TEXT')
  conn.execute_command('HSET', 'doc1', 't1', 'hello', 't2', 'hello')
  conn.execute_command('HSET', 'doc1', 't1', 'hello', 't2', 'hello')

  env.expect('ft.search', 'idx', '@t1:*ell*', 'NOCONTENT').equal([1, 'doc1'])
  env.expect('ft.search', 'idx', '@t2:*ell*', 'NOCONTENT').error()  \
    .contains('Contains query on fields without WITHSUFFIXTRIE support')

def test_params(env):
  env = Env(moduleArgs = 'DEFAULT_DIALECT 2')
  # this test check that `\*` is escaped correctly on contains queries
  conn = getConnectionByEnv(env)
  env.cmd('ft.create', 'idx', 'SCHEMA', 't', 'TEXT', 'SORTABLE')

  conn.execute_command('HSET', 'doc1', 't', 'world')

  env.expect('ft.search', 'idx', '$prefix*', 'PARAMS', 2, 'prefix', 'wor').equal([1, 'doc1', ['t', 'world']])
  env.expect('ft.search', 'idx', '*$contains*', 'PARAMS', 2, 'contains', 'orl').equal([1, 'doc1', ['t', 'world']])
  env.expect('ft.search', 'idx', '*$suffix', 'PARAMS', 2, 'suffix', 'rld').equal([1, 'doc1', ['t', 'world']])

@skip(cluster=True)
def test_issue_3124(env):
  # test prefix query on field with suffix trie
  env.cmd('ft.create', 'idx_txt', 'SCHEMA', 't', 'TEXT')
  env.cmd('ft.create', 'idx_txt_suffix', 'SCHEMA', 't', 'TEXT', 'WITHSUFFIXTRIE')
  env.cmd('ft.create', 'idx_tag', 'SCHEMA', 't', 'TAG')
  env.cmd('ft.create', 'idx_tag_suffix', 'SCHEMA', 't', 'TAG', 'WITHSUFFIXTRIE')
  conn = getConnectionByEnv(env)

  # insert a single document with value 'hello' in field 't'
  conn.execute_command('HSET', 'doc', 't', 'hello')

  # test prefix query on field with existing prefix query
  res_exist1 = env.cmd('ft.search', 'idx_txt', '@t:hell*')
  res_exist2 = env.cmd('ft.search', 'idx_txt_suffix', '@t:hell*')
  env.assertEqual(res_exist1, res_exist2)

  # test prefix query on field with non-existing prefix query
  res_not_exist1 = env.cmd('ft.search', 'idx_txt', '@t:ell*')
  res_not_exist2 = env.cmd('ft.search', 'idx_txt_suffix', '@t:ell*')
  env.assertEqual(res_not_exist1, res_not_exist2)
