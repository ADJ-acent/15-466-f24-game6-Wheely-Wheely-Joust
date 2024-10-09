#include "Game.hpp"

#include "Connection.hpp"
#include "data_path.hpp"
#define _USE_MATH_DEFINES
#include <math.h>

#include <stdexcept>
#include <iostream>
#include <cstring>

#include <glm/gtx/norm.hpp>

void Player::Controls::send_controls_message(Connection *connection_) const {
	assert(connection_);
	auto &connection = *connection_;

	uint32_t size = 10;
	connection.send(Message::C2S_Controls);
	connection.send(uint8_t(size));
	connection.send(uint8_t(size >> 8));
	connection.send(uint8_t(size >> 16));

	auto send_button = [&](Button const &b) {
		if (b.downs & 0x80) {
			std::cerr << "Wow, you are really good at pressing buttons!" << std::endl;
		}
		connection.send(uint8_t( (b.pressed ? 0x80 : 0x00) | (b.downs & 0x7f) ) );
	};

	send_button(left);
	send_button(right);
	send_button(up);
	send_button(down);
	send_button(jump);
	send_button(LMB);
	connection.send(mouse_x);
}

bool Player::Controls::recv_controls_message(Connection *connection_) {
	assert(connection_);
	auto &connection = *connection_;

	auto &recv_buffer = connection.recv_buffer;

	//expecting [type, size_low0, size_mid8, size_high8]:
	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::C2S_Controls)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	if (size != 10) throw std::runtime_error("Controls message with size " + std::to_string(size) + " != 10!");
	
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	auto recv_button = [](uint8_t byte, Button *button) {
		button->pressed = (byte & 0x80);
		uint32_t d = uint32_t(button->downs) + uint32_t(byte & 0x7f);
		if (d > 255) {
			std::cerr << "got a whole lot of downs" << std::endl;
			d = 255;
		}
		button->downs = uint8_t(d);
	};

	recv_button(recv_buffer[4+0], &left);
	recv_button(recv_buffer[4+1], &right);
	recv_button(recv_buffer[4+2], &up);
	recv_button(recv_buffer[4+3], &down);
	recv_button(recv_buffer[4+4], &jump);
	recv_button(recv_buffer[4+5], &LMB);
	mouse_x = reinterpret_cast<float*>(&recv_buffer[10])[0];

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}


//-----------------------------------------

Game::Game() {
	main_scene_server = Scene(data_path("arena.scene"), nullptr);
	reset_hamsters();
}

Player *Game::spawn_player() {
	if (next_player_number < 2) {
		player_ready[next_player_number] = true;
		return &players[next_player_number++];
	}
	else if (!player_ready[0]) {
		player_ready[0] = true;
		return &players[0];
	}
	else if (!player_ready[1]) {
		player_ready[1] = true;
		return &players[1];
	}
	return nullptr; // spectator is the nullptr
}

void Game::remove_player(Player *player) {
	if (player == nullptr) { // spectator has no effect on server
		return;
	}
	bool found = false;
	for (uint8_t i = 0; i < uint8_t(players.size()); ++i) {
		Player * cur_player = &players[i];
		if (cur_player == player) {
			player_ready[i] = false;
			found = true;
			break;
		}
	}
	assert(found);
}

void Game::update(float elapsed) {
	// cache last hamster position
	glm::vec3 hamster_last_pos[2] = {players[0].position, players[1].position};
	//position/velocity update:
	for (uint8_t i = 0; i<2; ++i) {
		auto &p = players[i];

		//handle attacking
		const float attack_LUT[11] = {0.0f,-1.0f,-1.2f,.1f,1.0f,3.0f, 2.4f, 1.8f, 1.2f, .6f, 0.0f};
		if (p.since_attack != 0.0f) {
			p.since_attack += elapsed;
			if (p.since_attack > 1.0f) {
				p.since_attack = 0.0f;
			}
			else {
				float res = p.since_attack / 0.1f;
				float interval = 0.0f;
				float t = std::modf(res, &interval);
				float attack_magintude;
				assert(interval >= 0.0f);
				if (interval >= 10.0f) {
					attack_magintude = 0.0f;
				}
				else {
					attack_magintude = attack_LUT[uint32_t(interval)] * (1.0f - t) + attack_LUT[uint32_t(interval) + 1] * t;
				}
				
				//due to some weirdness in my blender file, you need to reverse the directions of the spear for blue hamster...
				attack_magintude = i == 0 ? attack_magintude : -attack_magintude;
				p.lance_position = initial_player_state[i].lance_position + p.lance_rotation * glm::vec3(0.0f,attack_magintude,0.0f);
			}
		}
		else if (p.controls.LMB.downs != 0) {
			p.since_attack = 0.001f;
		}


		//rotating the hamster view using mouse
		if (p.controls.mouse_x != 0.0f) {
			p.rotation = glm::normalize(
				p.rotation
				* glm::angleAxis(-p.controls.mouse_x * 3.0f, glm::vec3(0.0f, 0.0f, 1.0f))
			);
		}

		//lance rotation, only applicable when not in attack or cooldown
		if (p.since_attack == 0.0f) {
			if (p.controls.jump.pressed) {
				static float direction = 1.0f;
				p.cur_lance_angle += direction * 90.0f * elapsed;
				if (p.cur_lance_angle > 60.0f) {
					p.cur_lance_angle = 60.0f;
					direction = -1.0f;
				}
				else if (p.cur_lance_angle < -15.0f) {
					p.cur_lance_angle = -15.0f;
					direction = 1.0f;
				}
				p.lance_rotation = initial_player_state[i].lance_rotation* glm::angleAxis(
					glm::radians(p.cur_lance_angle),
					glm::vec3(0.0f, 0.0f, 1.0f)
				);
			}
		}
		glm::vec3 dir = glm::vec3(0.0f);
		if (p.controls.down.pressed) dir.y += 1.0f;
		if (p.controls.up.pressed) dir.y -= 1.0f;
		if (p.controls.left.pressed) dir.x += 1.0f;
		if (p.controls.right.pressed) dir.x -= 1.0f;

		glm::vec3 wheel_rotation = dir;

		dir = p.rotation * dir;

		if (dir == glm::vec3(0.0f)) {
			//no inputs: just drift to a stop
			float amt = 1.0f - std::pow(0.5f, elapsed / (PlayerAccelHalflife * 2.0f));
			p.velocity = glm::mix(p.velocity, glm::vec3(0.0f), amt);
		} else {
			//inputs: tween velocity to target direction
			dir = glm::normalize(dir);

			float amt = 1.0f - std::pow(0.5f, elapsed / PlayerAccelHalflife);

			//accelerate along velocity (if not fast enough):
			float along = glm::dot(p.velocity, dir);
			if (along < PlayerSpeed) {
				along = glm::mix(along, PlayerSpeed, amt);
			}

			//damp perpendicular velocity:
			float perp = glm::dot(p.velocity, glm::vec3(-dir.y, dir.x, 0.0f));
			perp = glm::mix(perp, 0.0f, amt);

			p.velocity = dir * along + glm::vec3(-dir.y, dir.x, 0.0f) * perp;
		}
		p.position += p.velocity * elapsed;
		static float rotate_angle[2] = {0.0f, 0.0f};

		//spin the wheel based on velocity and input direction
		rotate_angle[i] -= glm::length(p.velocity) * elapsed * .5f;
		rotate_angle[i] = std::fmodf(rotate_angle[i], 360.0f);

		p.wheel_rotation = initial_player_state[i].wheel_rotation; 


		const static float amt = 1.0f - std::pow(0.5f, elapsed / (.1f * 2.0f));
		static float last_wheel_x_angle[2] = {0.0f,0.0f};
		
		if (wheel_rotation != glm::vec3(0.0f)) {
			float angle = 0.0f;
			if (wheel_rotation.x == 0.0f) {
				if (wheel_rotation.y == -1.0f) {
					angle = 0.0f;
				}
				else {
					angle = float(M_PI);
				}
			}
			else if (wheel_rotation.x == 1.0f) {
				if (wheel_rotation.y == -1.0f) {
					angle = 1.75f * float(M_PI);
				}
				else if (wheel_rotation.y == 1.0f) {
					angle = 1.25f * float(M_PI);
				}
				else {
					angle = 1.5f * float(M_PI);
				}
			}
			else {
				if (wheel_rotation.y == -1.0f) {
					angle = .25f * float(M_PI);
				}
				else if (wheel_rotation.y == 1.0f) {
					angle = .75f * float(M_PI);
				}
				else {
					angle = .5f * float(M_PI);
				}
			}
			// angle = abs(angle - float(M_PI) - last_wheel_x_angle[i]) <= abs(angle - last_wheel_x_angle[i]) ? angle - float(M_PI) : angle;
			// std::cout<< angle << ", "<<last_wheel_x_angle[i]<<std::endl;
			last_wheel_x_angle[i] = glm::mix(last_wheel_x_angle[i], angle, amt);
		}
		else {
			last_wheel_x_angle[i] = glm::mix(last_wheel_x_angle[i], 0.0f, amt);
		}
		p.wheel_rotation *= glm::angleAxis(last_wheel_x_angle[i], glm::vec3(1.0f, 0.0f, 0.0f)) * glm::angleAxis(
			rotate_angle[i],
			glm::vec3(0.0f, 0.0f, 1.0f)
		);

		// player/arena collisions:
		if (p.position.x < ArenaMin.x + PlayerRadius) {
			p.position.x = ArenaMin.x + PlayerRadius;
			p.velocity.x = std::abs(p.velocity.x) * 0.5f;
		}
		if (p.position.x > ArenaMax.x - PlayerRadius) {
			p.position.x = ArenaMax.x - PlayerRadius;
			p.velocity.x =-std::abs(p.velocity.x) * 0.5f;
		}
		if (p.position.y < ArenaMin.y + PlayerRadius) {
			p.position.y = ArenaMin.y + PlayerRadius;
			p.velocity.y = std::abs(p.velocity.y) * 0.5f;
		}
		if (p.position.y > ArenaMax.y - PlayerRadius) {
			p.position.y = ArenaMax.y - PlayerRadius;
			p.velocity.y =-std::abs(p.velocity.y) * 0.5f;
		}
		//reset 'downs' since controls have been handled:
		p.controls.left.downs = 0;
		p.controls.right.downs = 0;
		p.controls.up.downs = 0;
		p.controls.down.downs = 0;
		p.controls.jump.downs = 0;
		p.controls.LMB.downs = 0;
		p.controls.mouse_x = 0.0f;

	}

	//collision note: hamster is roughly a sphere with .9 radius, chair has roughly 1.1 radius

	// player on player collision resolution:
	for (auto &p1 : players) {
		//player/player collisions:
		for (auto &p2 : players) {
			if (&p1 == &p2) break;
			glm::vec3 p12 = p2.position - p1.position;
			float len2 = glm::length2(p12);
			if (len2 > (2.0f * PlayerRadius) * (2.0f * PlayerRadius)) continue;
			if (len2 == 0.0f) continue;
			glm::vec3 dir = p12 / std::sqrt(len2);
			//mirror velocity to be in separating direction:
			glm::vec3 v12 = p2.velocity - p1.velocity;
			glm::vec3 delta_v12 = dir * glm::max(0.0f, -1.75f * glm::dot(dir, v12));
			p2.velocity += 0.5f * delta_v12;
			p1.velocity -= 0.5f * delta_v12;
		}
	}
}


void Game::send_state_message(Connection *connection_, Player *connection_player) const {
	assert(connection_);
	auto &connection = *connection_;

	connection.send(Message::S2C_State);
	//will patch message size in later, for now placeholder bytes:
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	connection.send(uint8_t(0));
	size_t mark = connection.send_buffer.size(); //keep track of this position in the buffer


	//send player info helper:
	auto send_player = [&](Player const &player) {

		connection.send(player.dead);
		connection.send(player.health);
		connection.send(player.since_attack);
		connection.send(player.rotation);
		connection.send(player.lance_rotation);
		connection.send(player.velocity);
		connection.send(player.lance_position);
		connection.send(player.wheel_rotation);
		connection.send(player.position);
	
		//NOTE: can't just 'send(name)' because player.name is not plain-old-data type.
		//effectively: truncates player name to 255 chars
		// uint8_t len = uint8_t(std::min< size_t >(255, player.name.size()));
		// connection.send(len);
		// connection.send_buffer.insert(connection.send_buffer.end(), player.name.begin(), player.name.begin() + len);
	};

	//number of ready players
	connection.send(player_ready);
	// whether this player is red or blue hamster
	if (connection_player != nullptr) {
		connection.send(static_cast<PlayerType>(connection_player != &players[0]));
	}
	else {
		connection.send(PlayerType::Spectator);
	}
	for (auto const &player : players) {
		send_player(player);
	}

	//compute the message size and patch into the message header:
	uint32_t size = uint32_t(connection.send_buffer.size() - mark);
	connection.send_buffer[mark-3] = uint8_t(size);
	connection.send_buffer[mark-2] = uint8_t(size >> 8);
	connection.send_buffer[mark-1] = uint8_t(size >> 16);
}

void Game::reset_hamsters()
{
	static bool initialized = false;
	if (!initialized) {
		Player &red_hamster = initial_player_state[0];
		red_hamster.dead = false;
		red_hamster.health = 10;
		red_hamster.velocity = glm::vec3(0.0f);
		red_hamster.since_attack = 0.0f;
		red_hamster.position = {1.0f, -22.0f, 1.8477f};

		Player &blue_hamster = initial_player_state[1];
		blue_hamster.dead = false;
		blue_hamster.health = 10;
		blue_hamster.velocity = glm::vec3(0.0f);
		blue_hamster.since_attack = 0.0f;
		blue_hamster.position = {1.0f, 22.0f, 1.8477f};

		for (auto &transform : main_scene_server.transforms) {

			if (transform.name == "RedHamster") {
				red_hamster.rotation = transform.rotation;
			}
			else if (transform.name == "BlueHamster") {
				blue_hamster.rotation = transform.rotation;
			}
			else if (transform.name == "RedLance") {
				red_hamster.lance_rotation = transform.rotation;
				red_hamster.lance_position = transform.position;
			}
			else if (transform.name == "BlueLance") {
				blue_hamster.lance_rotation = transform.rotation;
				blue_hamster.lance_position = transform.position;
			}
			else if (transform.name == "RedWheel") {
				red_hamster.wheel_rotation = transform.rotation;
			}
			else if (transform.name == "BlueWheel") {
				blue_hamster.wheel_rotation = transform.rotation;
			}
			else if (transform.name == "RedLancePoint") {
				lance_tip_transform[0] = &transform;
			}
			else if (transform.name == "BlueLancePoint") {
				lance_tip_transform[1] = &transform;
			}
		}
		initialized = true;
	}
	players[0] = initial_player_state[0];
	players[1] = initial_player_state[1];
}

bool Game::sphere_point_intersection(const glm::vec3 &sphere_position, float sphere_radius, const glm::vec3 &point_position, const glm::vec3 &sphere_velocity, const glm::vec3 &point_velocity)
{
	glm::vec3 relative_velocity = point_velocity - sphere_velocity;
	glm::vec3 relative_position = point_position - sphere_position;

	// Using distance2 for efficiency (avoids square root)
	float distance_squared = glm::distance2(relative_position, glm::vec3(0.0f)); 

	return distance_squared <= sphere_radius * sphere_radius;
}

bool Game::recv_state_message(Connection *connection_)
{
    assert(connection_);
	auto &connection = *connection_;
	auto &recv_buffer = connection.recv_buffer;

	if (recv_buffer.size() < 4) return false;
	if (recv_buffer[0] != uint8_t(Message::S2C_State)) return false;
	uint32_t size = (uint32_t(recv_buffer[3]) << 16)
	              | (uint32_t(recv_buffer[2]) << 8)
	              |  uint32_t(recv_buffer[1]);
	uint32_t at = 0;
	//expecting complete message:
	if (recv_buffer.size() < 4 + size) return false;

	//copy bytes from buffer and advance position:
	auto read = [&](auto *val) {
		if (at + sizeof(*val) > size) {
			throw std::runtime_error("Ran out of bytes reading state message.");
		}
		std::memcpy(val, &recv_buffer[4 + at], sizeof(*val));
		at += sizeof(*val);
	};

	read(&player_ready);
	read(&player_type);
	for (uint8_t i = 0; i < 2; ++i) {
		Player &player = players[i];
		read(&player.dead);
		read(&player.health);
		read(&player.since_attack);
		read(&player.rotation);
		read(&player.lance_rotation);
		read(&player.velocity);
		read(&player.lance_position);
		read(&player.wheel_rotation);
		read(&player.position);
		// uint8_t name_len;
		// read(&name_len);
		//n.b. would probably be more efficient to directly copy from recv_buffer, but I think this is clearer:
		// player.name = "";
		// for (uint8_t n = 0; n < name_len; ++n) {
		// 	char c;
		// 	read(&c);
		// 	player.name += c;
		// }
	}
	assert(at==size);
	if (at != size) throw std::runtime_error("Trailing data in state message.");

	//delete message from buffer:
	recv_buffer.erase(recv_buffer.begin(), recv_buffer.begin() + 4 + size);

	return true;
}