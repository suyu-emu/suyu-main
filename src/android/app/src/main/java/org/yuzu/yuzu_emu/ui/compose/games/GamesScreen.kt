// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// Author: Ghael_V - Lemon Switch Emu - Suyu Team

package org.yuzu.yuzu_emu.ui.compose.games

import android.content.res.Configuration
import android.widget.ImageView
import androidx.compose.animation.core.animateFloatAsState
import androidx.compose.foundation.lazy.itemsIndexed
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.ExperimentalComposeUiApi
import androidx.compose.ui.focus.FocusRequester
import androidx.compose.ui.focus.focusProperties
import androidx.compose.ui.platform.LocalSoftwareKeyboardController
import androidx.compose.ui.focus.focusRequester
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.zIndex
import kotlinx.coroutines.delay
import androidx.compose.foundation.ExperimentalFoundationApi
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.combinedClickable
import androidx.compose.foundation.clickable
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.aspectRatio
import androidx.compose.foundation.layout.asPaddingValues
import androidx.compose.foundation.layout.fillMaxHeight
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyRow
import androidx.compose.foundation.lazy.grid.GridCells
import androidx.compose.foundation.lazy.grid.GridItemSpan
import androidx.compose.foundation.lazy.grid.LazyVerticalGrid
import androidx.compose.foundation.lazy.grid.items
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.BasicTextField
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.ui.focus.FocusDirection
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.onPreviewKeyEvent
import androidx.compose.ui.input.key.type
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Text
import androidx.compose.material3.pulltorefresh.PullToRefreshBox
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.SolidColor
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalConfiguration
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.integerResource
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import androidx.core.content.edit
import androidx.preference.PreferenceManager
import info.debatty.java.stringsimilarity.Jaccard
import info.debatty.java.stringsimilarity.JaroWinkler
import java.util.Locale
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.yuzu.yuzu_emu.R
import org.yuzu.yuzu_emu.model.Game
import org.yuzu.yuzu_emu.ui.compose.SuyuIcons
import org.yuzu.yuzu_emu.ui.compose.theme.SuyuColors
import org.yuzu.yuzu_emu.utils.GameIconUtils

private const val PREF_SORT_TYPE = "GamesSortType"
private const val FILTER_ALL = 1
private const val FILTER_RECENTLY_PLAYED = 2
private const val FILTER_RECENTLY_ADDED = 3
private const val FILTER_FAVORITES = -2
private const val DAY_MS = 24 * 60 * 60 * 1000L
private val HERO_COVER_SIZE = 200.dp
private val CAROUSEL_PAD = 10.dp
private val CAROUSEL_TEXT_HEIGHT = 64.dp
private const val CAROUSEL_FOCUS_SCALE = 1.14f

data class GameItem(val game: Game, val favorite: Boolean, val fps: Float?)

enum class LibraryDestination { LIBRARY, CONTROLS, DRIVERS, SETTINGS }

internal fun cleanTitle(title: String) = title.replace("[\\t\\n\\r]+".toRegex(), " ")

private fun filterGames(
    games: List<Game>,
    favorites: Set<String>,
    filter: Int,
    query: String
): List<Game> {
    val now = System.currentTimeMillis()
    val base = when (filter) {
        FILTER_FAVORITES -> games.filter { it.path in favorites }
        FILTER_RECENTLY_PLAYED -> games
            .filter { GameMeta.lastPlayed(it) > now - DAY_MS }
            .sortedByDescending { GameMeta.lastPlayed(it) }
        FILTER_RECENTLY_ADDED -> games
            .filter { GameMeta.addedToLibrary(it) > now - DAY_MS }
            .sortedByDescending { GameMeta.addedToLibrary(it) }
        FILTER_ALL -> games.sortedBy { it.title }
        else -> games
    }
    val term = query.lowercase(Locale.getDefault())
    if (term.isEmpty()) {
        return base
    }
    val algorithm = if (term.length > 1) Jaccard(2) else JaroWinkler()
    return base
        .map { it to algorithm.similarity(term, it.title.lowercase(Locale.getDefault())) }
        .filter { it.second > 0.03 }
        .sortedByDescending { it.second }
        .map { it.first }
}

@OptIn(ExperimentalMaterial3Api::class, ExperimentalFoundationApi::class)
@Composable
fun GamesScreen(
    games: List<Game>,
    favorites: Set<String>,
    isRefreshing: Boolean,
    refreshTick: Int,
    showQLaunch: Boolean,
    showAddFolder: Boolean,
    onRefresh: () -> Unit,
    onAddFolder: () -> Unit,
    onQLaunch: () -> Unit,
    onPlay: (Game) -> Unit,
    onProperties: (Game) -> Unit,
    onToggleFavorite: (Game) -> Unit,
    onNavigate: (LibraryDestination) -> Unit
) {
    val context = LocalContext.current
    val preferences = remember { PreferenceManager.getDefaultSharedPreferences(context) }
    val isLandscape = LocalConfiguration.current.orientation == Configuration.ORIENTATION_LANDSCAPE
    val viewTypeKey = if (isLandscape) {
        GamesViewType.PREF_LANDSCAPE
    } else {
        GamesViewType.PREF_PORTRAIT
    }

    var query by rememberSaveable { mutableStateOf("") }
    var filter by rememberSaveable {
        mutableIntStateOf(
            preferences.getInt(PREF_SORT_TYPE, FILTER_ALL).takeIf {
                it == FILTER_RECENTLY_PLAYED || it == FILTER_RECENTLY_ADDED
            } ?: FILTER_ALL
        )
    }
    var viewType by remember(viewTypeKey) {
        val fallback = if (isLandscape) GamesViewType.CAROUSEL else GamesViewType.GRID
        val saved = preferences.getInt(viewTypeKey, fallback)
        mutableIntStateOf(
            if (!isLandscape && saved == GamesViewType.CAROUSEL) GamesViewType.GRID else saved
        )
    }

    val shown = remember(games, favorites, filter, query, refreshTick) {
        filterGames(games, favorites, filter, query)
    }
    val items = remember(shown, favorites, refreshTick) {
        shown.map { GameItem(it, it.path in favorites, GameMeta.averageFps(it)) }
    }
    val hero = remember(games, refreshTick) {
        games.filter { GameMeta.lastPlayed(it) > 0L }.maxByOrNull { GameMeta.lastPlayed(it) }
    }

    val gridColumns = integerResource(R.integer.game_columns_grid)
    val listColumns = integerResource(R.integer.game_columns_list)
    val columns = when (viewType) {
        GamesViewType.LIST -> listColumns
        else -> gridColumns
    }
    val topInset = WindowInsets.statusBars.asPaddingValues().calculateTopPadding()

    val queryState = rememberUpdatedState(query)

    Column(
        modifier = Modifier
            .fillMaxSize()
            .background(SuyuColors.Background)
    ) {
        val onSelectFilter: (Int) -> Unit = {
            filter = it
            if (it != FILTER_FAVORITES) {
                preferences.edit { putInt(PREF_SORT_TYPE, it) }
            }
        }
        val onSelectViewType: (Int) -> Unit = {
            viewType = it
            preferences.edit { putInt(viewTypeKey, it) }
        }
        val header: @Composable () -> Unit = {
            Column(verticalArrangement = Arrangement.spacedBy(14.dp)) {
                BrandSearchRow(query = query, onQueryChange = { query = it })
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    FilterChips(
                        selected = filter,
                        modifier = Modifier.weight(1f),
                        onSelect = {
                            filter = it
                            if (it != FILTER_FAVORITES) {
                                preferences.edit { putInt(PREF_SORT_TYPE, it) }
                            }
                        }
                    )
                    ActionButtons(
                        showAddFolder = showAddFolder,
                        showQLaunch = showQLaunch,
                        viewType = viewType,
                        isLandscape = isLandscape,
                        onViewType = {
                            viewType = it
                            preferences.edit { putInt(viewTypeKey, it) }
                        },
                        onAddFolder = onAddFolder,
                        onQLaunch = onQLaunch
                    )
                }
            }
        }

        PullToRefreshBox(
            isRefreshing = isRefreshing,
            onRefresh = onRefresh,
            modifier = Modifier.weight(1f)
        ) {
            if (viewType == GamesViewType.CAROUSEL) {
                Column(
                    modifier = Modifier
                        .fillMaxSize()
                        .padding(top = topInset + 8.dp, start = 16.dp, end = 16.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp)
                ) {
                    // Landscape: logo, name, search, filters and actions share a single row so the
                    // covers get the height.
                    Row(
                        modifier = Modifier
                            .fillMaxWidth()
                            .height(52.dp),
                        verticalAlignment = Alignment.CenterVertically,
                        horizontalArrangement = Arrangement.spacedBy(10.dp)
                    ) {
                        Image(
                            painter = painterResource(R.drawable.ic_suyu_full),
                            contentDescription = null,
                            modifier = Modifier.size(40.dp)
                        )
                        Text(
                            text = stringResource(R.string.app_name),
                            color = SuyuColors.Text,
                            fontSize = 26.sp,
                            fontWeight = FontWeight.Bold,
                            maxLines = 1
                        )
                        SearchField(
                            query = query,
                            onQueryChange = { query = it },
                            modifier = Modifier.weight(1f)
                        )
                        FilterChips(
                            selected = filter,
                            onSelect = onSelectFilter,
                            modifier = Modifier.weight(1.6f)
                        )
                        ActionButtons(
                            showAddFolder = showAddFolder,
                            showQLaunch = showQLaunch,
                            viewType = viewType,
                            isLandscape = isLandscape,
                            onViewType = onSelectViewType,
                            onAddFolder = onAddFolder,
                            onQLaunch = onQLaunch
                        )
                    }
                    val firstFocus = remember { FocusRequester() }
                    // Highlight the first card when the list (re)loads, but never while the user is searching.
                    LaunchedEffect(items.firstOrNull()?.game?.path) {
                        if (items.isNotEmpty()) {
                            delay(300)
                            // Android gives the search field the first focus; cards come first here.
                            if (queryState.value.isEmpty()) {
                                runCatching { firstFocus.requestFocus() }
                            }
                        }
                    }
                    BoxWithConstraints(modifier = Modifier.fillMaxSize()) {
                        // Room for the focus scale; the cover takes whatever the text leaves.
                        val cardHeight = maxHeight / CAROUSEL_FOCUS_SCALE
                        val cover = cardHeight - CAROUSEL_TEXT_HEIGHT - CAROUSEL_PAD * 2
                        LazyRow(
                            modifier = Modifier.fillMaxSize(),
                            horizontalArrangement = Arrangement.spacedBy(4.dp),
                            verticalAlignment = Alignment.CenterVertically,
                            contentPadding = PaddingValues(horizontal = 20.dp)
                        ) {
                            itemsIndexed(items, key = { _, it -> it.game.path }) { index, item ->
                                GridCard(
                                    item = item,
                                    refreshTick = refreshTick,
                                    modifier = Modifier
                                        .width(cover + CAROUSEL_PAD * 2)
                                        .height(cardHeight)
                                        .then(if (index == 0) Modifier.focusRequester(firstFocus) else Modifier),
                                    focusScale = CAROUSEL_FOCUS_SCALE,
                                    innerPadding = CAROUSEL_PAD,
                                    onPlay = onPlay,
                                    onProperties = onProperties,
                                    onToggleFavorite = onToggleFavorite
                                )
                            }
                        }
                    }
                }
            } else {
                LazyVerticalGrid(
                    columns = GridCells.Fixed(columns),
                    modifier = Modifier.fillMaxSize(),
                    contentPadding = PaddingValues(
                        start = 16.dp,
                        end = 16.dp,
                        top = topInset + 8.dp,
                        bottom = 16.dp
                    ),
                    horizontalArrangement = Arrangement.spacedBy(14.dp),
                    verticalArrangement = Arrangement.spacedBy(14.dp)
                ) {
                    item(span = { GridItemSpan(maxLineSpan) }) { header() }
                    if (hero != null && query.isEmpty() && filter == FILTER_ALL) {
                        item(span = { GridItemSpan(maxLineSpan) }) {
                            Hero(game = hero, onPlay = onPlay)
                        }
                    }
                    item(span = { GridItemSpan(maxLineSpan) }) {
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            horizontalArrangement = Arrangement.SpaceBetween,
                            verticalAlignment = Alignment.Bottom
                        ) {
                            Text(
                                text = stringResource(R.string.games),
                                color = SuyuColors.Text,
                                fontSize = 18.sp,
                                fontWeight = FontWeight.SemiBold
                            )
                            Text(
                                text = stringResource(R.string.games_count, items.size),
                                color = SuyuColors.Muted,
                                fontSize = 13.sp
                            )
                        }
                    }
                    items(items, key = { it.game.path }) { item ->
                        when (viewType) {
                            GamesViewType.LIST -> ListCard(
                                item, refreshTick, onPlay, onProperties, onToggleFavorite
                            )
                            GamesViewType.GRID_COMPACT -> CompactCard(
                                item, onPlay, onProperties
                            )
                            else -> GridCard(
                                item = item,
                                refreshTick = refreshTick,
                                modifier = Modifier.fillMaxWidth(),
                                onPlay = onPlay,
                                onProperties = onProperties,
                                onToggleFavorite = onToggleFavorite
                            )
                        }
                    }
                }
            }
        }

        NavBar(selected = LibraryDestination.LIBRARY, onSelect = onNavigate)
    }
}

/** Original suyu logo, app name and search share one row. */
@Composable
private fun BrandSearchRow(query: String, onQueryChange: (String) -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .height(52.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Image(
            painter = painterResource(R.drawable.ic_suyu_full),
            contentDescription = null,
            modifier = Modifier.size(40.dp)
        )
        Text(
            text = stringResource(R.string.app_name),
            color = SuyuColors.Text,
            fontSize = 26.sp,
            fontWeight = FontWeight.Bold,
            maxLines = 1
        )
        SearchField(
            query = query,
            onQueryChange = onQueryChange,
            modifier = Modifier.weight(1f)
        )
    }
}

@Composable
private fun ActionButtons(
    showAddFolder: Boolean,
    showQLaunch: Boolean,
    viewType: Int,
    isLandscape: Boolean,
    onViewType: (Int) -> Unit,
    onAddFolder: () -> Unit,
    onQLaunch: () -> Unit
) {
    Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
        if (showQLaunch) {
            RoundIconButton(
                description = stringResource(R.string.qlaunch_applet),
                onClick = onQLaunch
            ) {
                Icon(
                    painter = painterResource(R.drawable.ic_applet),
                    contentDescription = null,
                    tint = SuyuColors.Text
                )
            }
        }
        ViewTypeButton(viewType, isLandscape, onViewType)
        if (showAddFolder) {
            RoundIconButton(
                description = stringResource(R.string.add_games_folder),
                onClick = onAddFolder
            ) { Icon(SuyuIcons.Add, contentDescription = null, tint = SuyuColors.Text) }
        }
    }
}

@Composable
private fun ViewTypeButton(viewType: Int, isLandscape: Boolean, onViewType: (Int) -> Unit) {
    var open by remember { mutableStateOf(false) }
    Box {
        RoundIconButton(description = stringResource(R.string.view_grid), onClick = { open = true }) {
            Icon(SuyuIcons.Library, contentDescription = null, tint = SuyuColors.Text)
        }
        DropdownMenu(expanded = open, onDismissRequest = { open = false }) {
            val options = buildList {
                add(GamesViewType.GRID to R.string.view_grid)
                add(GamesViewType.GRID_COMPACT to R.string.view_grid_compact)
                add(GamesViewType.LIST to R.string.view_list)
                if (isLandscape) add(GamesViewType.CAROUSEL to R.string.view_carousel)
            }
            options.forEach { (type, label) ->
                DropdownMenuItem(
                    text = {
                        Text(
                            text = stringResource(label),
                            color = if (type == viewType) SuyuColors.Accent else SuyuColors.Text
                        )
                    },
                    onClick = {
                        open = false
                        onViewType(type)
                    }
                )
            }
        }
    }
}

@Composable
private fun RoundIconButton(
    description: String,
    onClick: () -> Unit,
    content: @Composable () -> Unit
) {
    Box(
        modifier = Modifier
            .size(44.dp)
            .clip(CircleShape)
            .background(SuyuColors.SurfaceHigh)
            .clickable(role = Role.Button, onClick = onClick)
            .semantics { contentDescription = description },
        contentAlignment = Alignment.Center
    ) { content() }
}

/**
 * The field can be reached with the D-pad (it shows a focus ring) but only starts editing, and
 * brings up the keyboard, when it is clicked. Up, Down, Back or the keyboard's Done key leave
 * editing again, so the D-pad is never trapped inside the text field.
 */
@OptIn(ExperimentalComposeUiApi::class)
@Composable
private fun SearchField(query: String, onQueryChange: (String) -> Unit, modifier: Modifier = Modifier) {
    var editing by remember { mutableStateOf(false) }
    var rowFocused by remember { mutableStateOf(false) }
    val textRequester = remember { FocusRequester() }
    val rowRequester = remember { FocusRequester() }
    val keyboard = LocalSoftwareKeyboardController.current
    val focusManager = LocalFocusManager.current

    LaunchedEffect(editing) {
        if (editing) {
            runCatching { textRequester.requestFocus() }
            keyboard?.show()
        }
    }

    val stopEditing: (FocusDirection?) -> Unit = { direction ->
        editing = false
        keyboard?.hide()
        if (direction == null) {
            runCatching { rowRequester.requestFocus() }
        } else {
            focusManager.moveFocus(direction)
        }
    }

    val shape = RoundedCornerShape(26.dp)
    val ring = if (editing || rowFocused) SuyuColors.Accent else Color(0xFF232836)
    Row(
        modifier = modifier
            .fillMaxWidth()
            .height(48.dp)
            .clip(shape)
            .background(Color(0xFF151922))
            .border(if (editing || rowFocused) 2.dp else 1.dp, ring, shape)
            .focusRequester(rowRequester)
            .onFocusChanged { rowFocused = it.isFocused }
            .clickable(role = Role.Button) { editing = true }
            .padding(start = 14.dp, end = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(10.dp)
    ) {
        Icon(SuyuIcons.Search, contentDescription = null, tint = SuyuColors.Muted, modifier = Modifier.size(20.dp))
        Box(modifier = Modifier.weight(1f)) {
            if (query.isEmpty()) {
                Text(
                    text = stringResource(R.string.home_search_games),
                    color = SuyuColors.Muted,
                    fontSize = 15.sp,
                    maxLines = 1
                )
            }
            BasicTextField(
                value = query,
                onValueChange = onQueryChange,
                singleLine = true,
                textStyle = MaterialTheme.typography.bodyLarge.copy(color = SuyuColors.Text, fontSize = 15.sp),
                cursorBrush = SolidColor(SuyuColors.Accent),
                keyboardOptions = KeyboardOptions(imeAction = ImeAction.Done),
                keyboardActions = KeyboardActions(onDone = { stopEditing(null) }),
                modifier = Modifier
                    .fillMaxWidth()
                    .focusRequester(textRequester)
                    .focusProperties { canFocus = editing }
                    .onFocusChanged { if (!it.isFocused) editing = false }
                    .onPreviewKeyEvent { event ->
                        if (event.type != KeyEventType.KeyDown) {
                            false
                        } else {
                            when (event.key) {
                                Key.DirectionDown -> { stopEditing(FocusDirection.Down); true }
                                Key.DirectionUp -> { stopEditing(FocusDirection.Up); true }
                                Key.Back, Key.Escape -> { stopEditing(null); true }
                                else -> false
                            }
                        }
                    }
            )
        }
        if (query.isNotEmpty()) {
            Box(
                modifier = Modifier
                    .size(44.dp)
                    .clip(CircleShape)
                    .focusProperties { canFocus = false }
                    .clickable(role = Role.Button) { onQueryChange("") }
                    .semantics { contentDescription = "clear" },
                contentAlignment = Alignment.Center
            ) {
                Icon(SuyuIcons.Close, contentDescription = null, tint = SuyuColors.Muted, modifier = Modifier.size(20.dp))
            }
        }
    }
}

@Composable
private fun FilterChips(selected: Int, onSelect: (Int) -> Unit, modifier: Modifier = Modifier) {
    val chips = listOf(
        FILTER_ALL to R.string.filter_all,
        FILTER_FAVORITES to R.string.filter_favorites,
        FILTER_RECENTLY_PLAYED to R.string.search_recently_played,
        FILTER_RECENTLY_ADDED to R.string.search_recently_added
    )
    Row(
        modifier = modifier.horizontalScroll(rememberScrollState()),
        horizontalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        chips.forEach { (id, label) ->
            val on = id == selected
            val shape = RoundedCornerShape(22.dp)
            Box(
                modifier = Modifier
                    .height(44.dp)
                    .clip(shape)
                    .background(if (on) SuyuColors.Accent.copy(alpha = 0.18f) else Color.Transparent)
                    .border(1.dp, if (on) SuyuColors.Accent.copy(alpha = 0.5f) else SuyuColors.Outline, shape)
                    .clickable(role = Role.Button) { onSelect(id) }
                    .padding(horizontal = 18.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = stringResource(label),
                    color = if (on) SuyuColors.Accent else SuyuColors.TextSoft,
                    fontSize = 14.sp,
                    fontWeight = FontWeight.SemiBold
                )
            }
        }
    }
}

/** Highlighted game: its cover is deliberately larger than the grid cards. */
@Composable
private fun Hero(game: Game, onPlay: (Game) -> Unit) {
    val shape = RoundedCornerShape(28.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .focusHighlight(1.04f)
            .clip(shape)
            .background(SuyuColors.Surface)
            .border(1.dp, Color(0xFF232836), shape)
            .padding(14.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(18.dp)
    ) {
        GameCover(
            game,
            Modifier
                .size(HERO_COVER_SIZE)
                .clip(RoundedCornerShape(22.dp))
                .background(SuyuColors.SurfaceHigh)
        )
        Column(
            modifier = Modifier.weight(1f),
            verticalArrangement = Arrangement.spacedBy(6.dp)
        ) {
            Text(
                text = stringResource(R.string.recently_played_hero),
                color = SuyuColors.Accent,
                fontSize = 12.sp,
                fontWeight = FontWeight.SemiBold,
                modifier = Modifier
                    .clip(RoundedCornerShape(10.dp))
                    .background(SuyuColors.Accent.copy(alpha = 0.14f))
                    .padding(horizontal = 10.dp, vertical = 4.dp)
            )
            Text(
                text = cleanTitle(game.title),
                color = SuyuColors.Text,
                fontSize = 22.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 3,
                overflow = TextOverflow.Ellipsis
            )
            Text(text = game.developer, color = SuyuColors.Muted, fontSize = 13.sp, maxLines = 1)
            Box(
                modifier = Modifier
                    .padding(top = 6.dp)
                    .height(48.dp)
                    .clip(RoundedCornerShape(24.dp))
                    .background(SuyuColors.Accent)
                    .clickable(role = Role.Button) { onPlay(game) }
                    .padding(horizontal = 20.dp)
                    .semantics { contentDescription = game.title },
                contentAlignment = Alignment.Center
            ) {
                Row(verticalAlignment = Alignment.CenterVertically, horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Icon(SuyuIcons.Play, contentDescription = null, tint = SuyuColors.OnAccent, modifier = Modifier.size(22.dp))
                    Text(
                        text = stringResource(R.string.play),
                        color = SuyuColors.OnAccent,
                        fontWeight = FontWeight.Bold,
                        fontSize = 15.sp
                    )
                }
            }
        }
    }
}

/**
 * Focused cards (gamepad/D-pad on handhelds, or after a tap) scale up, rise above their
 * neighbours and get an accent ring. Must be applied before the clickable modifier.
 */
@Composable
internal fun Modifier.focusHighlight(
    focusedScale: Float,
    innerPadding: Dp = 6.dp,
    ringRadius: Dp = 32.dp
): Modifier {
    var focused by remember { mutableStateOf(false) }
    val scale by animateFloatAsState(
        targetValue = if (focused) focusedScale else 1f,
        label = "focusScale"
    )
    return this
        .onFocusChanged { focused = it.isFocused }
        .zIndex(if (focused) 1f else 0f)
        .graphicsLayer {
            scaleX = scale
            scaleY = scale
        }
        .then(
            if (focused) {
                Modifier
                    .background(SuyuColors.Accent.copy(alpha = 0.10f), RoundedCornerShape(ringRadius))
                    .border(2.dp, SuyuColors.Accent, RoundedCornerShape(ringRadius))
            } else {
                Modifier
            }
        )
        .padding(innerPadding)
}

@Composable
internal fun GameCover(game: Game, modifier: Modifier = Modifier) {
    AndroidView(
        factory = { ctx -> ImageView(ctx).apply { scaleType = ImageView.ScaleType.CENTER_CROP } },
        update = { GameIconUtils.loadGameIcon(game, it) },
        modifier = modifier
    )
}

@Composable
private fun metaText(item: GameItem, refreshTick: Int): String {
    val size by produceState<String?>(null, item.game.path, refreshTick) {
        value = withContext(Dispatchers.IO) { GameMeta.sizeLabel(item.game) }
    }
    val fps = item.fps?.let { String.format(Locale.US, "%.0f FPS", it) }
    return listOfNotNull(size, fps).joinToString(" · ").ifEmpty { item.game.developer }
}

@Composable
internal fun CompatBadge(fps: Float?, modifier: Modifier = Modifier) {
    val compat = GameMeta.compatFor(fps) ?: return
    val color = when (compat) {
        Compat.PERFECT -> SuyuColors.CompatPerfect
        Compat.GREAT -> SuyuColors.CompatGreat
        Compat.PLAYABLE -> SuyuColors.CompatPlayable
    }
    Text(
        text = stringResource(compat.labelRes),
        color = SuyuColors.Background,
        fontSize = 11.sp,
        fontWeight = FontWeight.SemiBold,
        modifier = modifier
            .clip(RoundedCornerShape(10.dp))
            .background(color)
            .padding(horizontal = 9.dp, vertical = 4.dp)
    )
}

@Composable
private fun FavoriteButton(item: GameItem, onToggle: (Game) -> Unit, modifier: Modifier = Modifier) {
    val icon: ImageVector = if (item.favorite) SuyuIcons.HeartFilled else SuyuIcons.Heart
    Box(
        modifier = modifier
            .size(44.dp)
            .clip(CircleShape)
            .clickable(role = Role.Button) { onToggle(item.game) }
            .semantics { contentDescription = "favorite" },
        contentAlignment = Alignment.Center
    ) {
        Icon(
            imageVector = icon,
            contentDescription = null,
            tint = if (item.favorite) SuyuColors.Heart else Color.White,
            modifier = Modifier.size(22.dp)
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun GridCard(
    item: GameItem,
    refreshTick: Int,
    modifier: Modifier,
    focusScale: Float = 1.08f,
    innerPadding: Dp = 6.dp,
    onPlay: (Game) -> Unit,
    onProperties: (Game) -> Unit,
    onToggleFavorite: (Game) -> Unit
) {
    Column(
        modifier = modifier.focusHighlight(focusScale, innerPadding).combinedClickable(
            role = Role.Button,
            onClick = { onPlay(item.game) },
            onLongClick = { onProperties(item.game) }
        ),
        verticalArrangement = Arrangement.spacedBy(8.dp)
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(1f)
                .clip(RoundedCornerShape(22.dp))
                .background(SuyuColors.SurfaceHigh)
        ) {
            GameCover(item.game, Modifier.fillMaxSize())
            CompatBadge(item.fps, Modifier.padding(10.dp))
            FavoriteButton(item, onToggleFavorite, Modifier.align(Alignment.TopEnd))
        }
        Column(modifier = Modifier.padding(horizontal = 4.dp)) {
            Text(
                text = cleanTitle(item.game.title),
                color = SuyuColors.Text,
                fontSize = 14.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
            Text(
                text = metaText(item, refreshTick),
                color = SuyuColors.Muted,
                fontSize = 12.sp,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis
            )
        }
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun CompactCard(item: GameItem, onPlay: (Game) -> Unit, onProperties: (Game) -> Unit) {
    Column(
        modifier = Modifier.focusHighlight(1.12f).combinedClickable(
            role = Role.Button,
            onClick = { onPlay(item.game) },
            onLongClick = { onProperties(item.game) }
        ),
        verticalArrangement = Arrangement.spacedBy(6.dp)
    ) {
        Box(
            modifier = Modifier
                .fillMaxWidth()
                .aspectRatio(1f)
                .clip(RoundedCornerShape(18.dp))
                .background(SuyuColors.SurfaceHigh)
        ) {
            GameCover(item.game, Modifier.fillMaxSize())
            CompatBadge(item.fps, Modifier.padding(8.dp))
        }
        Text(
            text = cleanTitle(item.game.title),
            color = SuyuColors.Text,
            fontSize = 13.sp,
            fontWeight = FontWeight.Medium,
            maxLines = 1,
            overflow = TextOverflow.Ellipsis,
            modifier = Modifier.padding(horizontal = 4.dp)
        )
    }
}

@OptIn(ExperimentalFoundationApi::class)
@Composable
private fun ListCard(
    item: GameItem,
    refreshTick: Int,
    onPlay: (Game) -> Unit,
    onProperties: (Game) -> Unit,
    onToggleFavorite: (Game) -> Unit
) {
    val shape = RoundedCornerShape(22.dp)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .clip(shape)
            .background(SuyuColors.Surface)
            .border(1.dp, Color(0xFF232836), shape)
            .combinedClickable(
                role = Role.Button,
                onClick = { onPlay(item.game) },
                onLongClick = { onProperties(item.game) }
            )
            .padding(10.dp),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(14.dp)
    ) {
        GameCover(
            item.game,
            Modifier
                .size(72.dp)
                .clip(RoundedCornerShape(16.dp))
        )
        Column(modifier = Modifier.weight(1f), verticalArrangement = Arrangement.spacedBy(2.dp)) {
            Text(
                text = cleanTitle(item.game.title),
                color = SuyuColors.Text,
                fontSize = 15.sp,
                fontWeight = FontWeight.SemiBold,
                maxLines = 2,
                overflow = TextOverflow.Ellipsis
            )
            Text(text = item.game.developer, color = SuyuColors.Muted, fontSize = 12.sp, maxLines = 1)
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                CompatBadge(item.fps)
                Text(
                    text = metaText(item, refreshTick),
                    color = SuyuColors.Muted,
                    fontSize = 12.sp,
                    maxLines = 1
                )
            }
        }
        FavoriteButton(item, onToggleFavorite)
    }
}

@Composable
internal fun NavBar(selected: LibraryDestination, onSelect: (LibraryDestination) -> Unit) {
    val entries = listOf(
        Triple(LibraryDestination.LIBRARY, SuyuIcons.Library, R.string.nav_library),
        Triple(LibraryDestination.CONTROLS, SuyuIcons.Controls, R.string.preferences_controls),
        Triple(LibraryDestination.DRIVERS, SuyuIcons.Drivers, R.string.nav_drivers),
        Triple(LibraryDestination.SETTINGS, SuyuIcons.Settings, R.string.preferences_settings)
    )
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(SuyuColors.Surface)
            .border(1.dp, Color(0xFF232836))
            .padding(
                bottom = WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding(),
                start = 8.dp,
                end = 8.dp
            )
            .height(68.dp),
        horizontalArrangement = Arrangement.SpaceAround,
        verticalAlignment = Alignment.CenterVertically
    ) {
        entries.forEach { (dest, icon, label) ->
            val on = dest == selected
            Column(
                modifier = Modifier
                    .width(80.dp)
                    .clickable(role = Role.Tab) { onSelect(dest) },
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Box(
                    modifier = Modifier
                        .width(64.dp)
                        .height(32.dp)
                        .clip(RoundedCornerShape(16.dp))
                        .background(if (on) SuyuColors.Accent.copy(alpha = 0.2f) else Color.Transparent),
                    contentAlignment = Alignment.Center
                ) {
                    Icon(
                        icon,
                        contentDescription = null,
                        tint = if (on) SuyuColors.Accent else SuyuColors.Muted,
                        modifier = Modifier.size(24.dp)
                    )
                }
                Text(
                    text = stringResource(label),
                    color = if (on) SuyuColors.Text else SuyuColors.Muted,
                    fontSize = 12.sp,
                    fontWeight = if (on) FontWeight.SemiBold else FontWeight.Medium,
                    maxLines = 1
                )
            }
        }
    }
}
